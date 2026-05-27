# Real RDMA CXL Sidecar Design

Date: 2026-05-26
Branch: `rdma-cxl-parallel-hybrid`
Supersedes: `docs/superpowers/specs/2026-05-23-rdma-cxl-parallel-hybrid-design.md`

## Goal

Replace the measurement-only RDMA sidecar with a real RDMA verbs sidecar that
moves selected whole CXL fault regions over an independent RDMA PCIe path while
the main migration stream remains CXL hybrid.

The required end state is:

- CXL remains the primary migration and postcopy visibility path.
- RDMA is an asynchronous secondary stream, not the main migration channel.
- RDMA moves only selected whole regions that have not already been migrated or
  made visible by CXL.
- RDMA completion marks source-authoritative `RDMA-ready` state only.
- Switch/postcopy visibility is still committed through the existing CXL hybrid
  control path after final dirty synchronization.
- The previous `memcpy` and shadow-copy sidecar paths are removed.

## Current Problem

The current `hybrid_parallel_rdma_cxl` implementation is useful only as a state
and metric spike. It does not exercise the real hardware path:

- `migration/cxl-rdma.c` completes sidecar work with local `memcpy()`.
- The script keeps the main migration URI as `unix:` and does not create a real
  sidecar RDMA connection.
- The current scheduler lets the sidecar own large contiguous regions and can
  make most bulk data appear as RDMA-side work.

That cannot validate the intended benefit. The expected benefit depends on CXL
and RDMA using independent PCIe paths concurrently. A local copy into mapped CXL
backing consumes CPU and CXL-side memory bandwidth, so it hides the distinction
between the two hardware paths.

## Architecture

Use a dedicated sidecar RDMA endpoint for `hybrid_parallel_rdma_cxl`.

The main migration channel remains the existing CXL hybrid channel, usually
`unix:<migration-sock>` in the experiment script. The sidecar opens a separate
RDMA CM/verbs connection using the same address model as native QEMU RDMA:
`rdma:<host>:<port>` parsed into the existing `MigrationAddress` / `rdma`
`InetSocketAddress` representation. It also reuses the existing
`rdma-pin-all` capability for pinning behavior.

The sidecar must not consume the main migration URI. Native RDMA uses the main
URI to make RDMA the RAM migration transport. Hybrid RDMA+CXL needs the same
RDMA address semantics for a secondary connection while the main URI remains
CXL hybrid.

Destination setup:

- Start the sidecar listener when CXL hybrid mapped backing is initialized and
  `x-cxl-rdma-sidecar=true`.
- Register the destination mapped CXL backing range with ibverbs.
- Send the remote base address, rkey, region granule, and protocol version to
  the source over the sidecar control connection.

Source setup:

- Connect to the destination sidecar endpoint after CXL hybrid source state is
  initialized.
- Register source RAM regions as needed, or pin all when the RDMA pin-all option
  is enabled.
- Post `IBV_WR_RDMA_WRITE` work requests for selected whole regions.
- On RDMA completion, mark the region ready only if it is still current and has
  not already been migrated or published by CXL.

The implementation may share concepts with QEMU native RDMA, but it must not
turn the main CXL migration URI into `rdma:`. Native RDMA owns the main RAM save
path and is not the desired model for this work.

## Sidecar Scheduling

RDMA is opportunistic. It must not block or starve the CXL main stream.

The sidecar owns a worker thread with a small in-flight window. For this
revision, RDMA only participates in the bulk phase. Bulk-phase RAM migration
starts from pages that are dirty in the migration bitmap, so dirty state is the
source of work, not a reason to skip a region.

When the worker has capacity, it selects one pending whole region that satisfies
all of these conditions:

- region is inside the mapped CXL backing range;
- region is not already CXL-visible;
- region has not already been fully migrated by CXL;
- region is not remapped;
- region has pending migration work in the current bulk dirty bitmap view;
- region is not already in RDMA in-flight, ready, committed, or invalidated
  state;
- total accepted RDMA coverage remains within the configured sidecar budget.

RDMA claim must be synchronized with the migration bitmap. Claiming a region
captures the current source contents as the RDMA transfer snapshot and clears or
records the corresponding pending bits for that region under the same locking
discipline used by RAM bulk migration. Any guest write after that claim sets the
region dirty again; completion then becomes stale or a later final dirty sync
invalidates the ready region.

Default policy:

- `x-cxl-rdma-sidecar-max-inflight-regions=1`
- `x-cxl-rdma-sidecar-max-cover-percent=25`
- `x-cxl-rdma-sidecar-region-bytes=0`, meaning use the CXL fault region granule

The defaults intentionally keep CXL as the dominant path. Experiments can raise
the limits to test saturation, but reports must state the configured budget.

RDMA in-flight state does not make CXL wait. If the CXL path migrates or
publishes a region before RDMA completes, CXL wins and the later RDMA completion
is counted as stale. This avoids the current behavior where RDMA ownership can
remove too much work from the CXL bulk path.

## State Model

Use region-level source-authoritative state:

- `rdma_candidate`: selected by the scheduler.
- `rdma_inflight`: RDMA write posted and not completed.
- `rdma_ready`: RDMA write completed and still current.
- `rdma_stale`: RDMA work completed after the claimed snapshot was dirtied
  again or after CXL already published the region.
- `rdma_committed`: switch-time final dirty sync kept the ready region current
  and CXL control visibility was published.
- `rdma_invalidated`: a ready region became dirty before commit.

CXL state remains authoritative for destination consumption:

- RDMA-ready does not make a page visible to the destination guest.
- `cxl_hybrid_commit_rdma_ready_regions_for_postcopy()` or its successor commits
  only ready and still-current regions.
- Dirty pages after RDMA claim or completion invalidate the whole region in v1.
- Invalidated or stale RDMA regions fall back to normal CXL postcopy region
  publication.

## Data Flow

1. Destination initializes CXL mapped backing and registers it as the RDMA remote
   memory range.
2. Source initializes CXL hybrid state and connects the RDMA sidecar endpoint.
3. The normal CXL bulk path continues migrating pages and publishing CXL state.
4. The RDMA worker opportunistically claims a pending bulk region, captures that
   source snapshot in the migration dirty state, and posts a whole-region RDMA
   write.
5. If RDMA completes before the claimed snapshot is dirtied again and before CXL
   publishes the region, the source marks it `rdma_ready`.
6. If CXL migrates or publishes the region first, the RDMA completion becomes
   stale and does not affect visibility.
7. At postcopy switch, final dirty sync invalidates ready regions dirtied after
   RDMA claim or completion.
8. Still-ready regions are committed into the CXL visibility contract.
9. Destination faults for all non-visible regions continue to use CXL postcopy
   region publication.

## Error Handling

Sidecar enablement is explicit. If `x-cxl-rdma-sidecar=true` and QEMU is built
without RDMA support, or if the sidecar address is missing or not an RDMA
address, migration parameter validation fails before migration starts.

If the sidecar listener, connect, memory registration, or QP setup fails,
`hybrid_parallel_rdma_cxl` fails explicitly. It must not silently fall back to
`memcpy`, shadow copy, or a fake ready state.

If an RDMA write fails after migration has started, the source drops the affected
region from RDMA state and lets CXL handle it. The failure is counted and traced.
If the transport enters a fatal error state, the sidecar stops scheduling new
regions and reports `rdma-sidecar-failed=true`; CXL remains responsible for
correctness.

## Metrics And Trace

Existing counters remain, but their meaning changes:

- `rdma_bulk_regions`: RDMA writes posted or completed by the real sidecar,
  never local copies.
- `rdma_bulk_bytes`: bytes posted or completed by the real sidecar.
- `rdma_ready_regions` / `rdma_ready_pages`: completed and still-current RDMA
  regions.
- `rdma_invalidated_regions` / `rdma_ready_pages_lost`: ready regions lost to
  later dirtying.
- `cxl_republish_*_due_to_rdma_invalidate`: CXL fallback publication for
  invalidated RDMA-ready regions.

Add sidecar-specific counters:

- `rdma_sidecar_connect_time_ns`
- `rdma_sidecar_registered_bytes`
- `rdma_sidecar_posted_regions`
- `rdma_sidecar_completed_regions`
- `rdma_sidecar_stale_regions`
- `rdma_sidecar_cxl_race_lost_regions`
- `rdma_sidecar_failed_regions`
- `rdma_sidecar_no_candidate_events`
- `rdma_sidecar_budget_skip_events`
- `rdma_sidecar_max_inflight_regions`
- `rdma_sidecar_max_cover_percent`

Trace events must distinguish scheduling, post, completion, stale completion,
transport failure, and commit. Reports should compare:

- CXL publish pages/time;
- RDMA posted/completed/stale bytes;
- postcopy region wait;
- guest stall;
- total time and downtime.

## Experiment Script

For `hybrid_parallel_rdma_cxl`, the script keeps the main migration URI as
`unix:<mig-sock>` and passes a native RDMA-style sidecar address through
migration params.

Use the same user-facing options as native RDMA where possible:

- `--rdma-host`
- `--rdma-port`
- `--rdma-pin-all`

The script builds the same address shape that native RDMA would derive from
`rdma:<host>:<port>`, but stores it in a sidecar-only parameter instead of using
it as the main migration URI. The preferred QMP shape is:

```json
{
  "transport": "rdma",
  "rdma": {
    "host": "<--rdma-host>",
    "port": "<--rdma-port plus run offset>"
  }
}
```

The script sets these QEMU controls for the sidecar mode:

- `x-cxl-rdma-sidecar=true`
- `x-cxl-rdma-sidecar-address=<native RDMA-style MigrationAddress>`
- `rdma-pin-all=<--rdma-pin-all>` using the existing migration capability
- `x-cxl-rdma-sidecar-max-inflight-regions=1` unless overridden
- `x-cxl-rdma-sidecar-max-cover-percent=25` unless overridden

Native RDMA precopy continues to use `rdma:<host>:<port>` as its main migration
URI. Hybrid RDMA+CXL does not.

## Testing Strategy

Unit tests:

- enabling sidecar without `CONFIG_RDMA` fails validation;
- sidecar parameter validation accepts a native RDMA-style address and rejects
  missing or non-RDMA addresses;
- `rdma-pin-all` uses the existing migration capability for both native RDMA and
  the sidecar connection;
- region selector accepts pending dirty bulk regions and skips visible,
  migrated, remapped, non-pending, in-flight, ready, invalidated, and
  over-budget regions;
- RDMA claim records or clears the selected region's current bulk dirty bitmap
  state so later guest writes can be detected as new dirties;
- CXL can publish a region while RDMA is in flight, and CXL wins the race;
- RDMA completion after CXL publication is counted stale and not committed;
- dirtying a ready RDMA region invalidates the whole region;
- commit exposes only still-current ready regions;
- CXL fallback republish accounting increments for invalidated ready regions.

Script tests:

- `hybrid_parallel_rdma_cxl` keeps the main migration URI as `unix:`;
- sidecar mode converts RDMA host/port into the native RDMA-style sidecar
  address and uses the existing `rdma-pin-all` capability;
- native RDMA precopy still uses `rdma:host:port`;
- output includes the new sidecar counters;
- summary/report fields distinguish CXL publish bytes from RDMA bytes.

Integration tests:

- with RDMA hardware available, a one-repeat no-brake run completes for
  `hybrid_postcopy_auto,hybrid_parallel_rdma_cxl`;
- trace shows sidecar RDMA post/completion events;
- QMP counters show real RDMA sidecar bytes and non-zero CXL publish work;
- no trace or symbol path reports local `memcpy` completion for the sidecar.

## Acceptance Criteria

The feature is acceptable when:

- `hybrid_parallel_rdma_cxl` cannot report RDMA completion without a real RDMA
  connection and verbs write completion;
- CXL remains the primary path under default settings;
- RDMA completed bytes stay within the configured coverage budget;
- RDMA in-flight regions do not block CXL publication;
- invalidated RDMA-ready regions are republished by CXL and correctly counted;
- reports no longer describe the old memcpy/shadow implementation as RDMA.

## Non-Goals

- No source-side brake integration in this revision.
- No sub-region RDMA/CXL interleaving before the whole-region policy is measured.
- No silent fallback from RDMA sidecar to `memcpy` or shadow copy.
- No conversion of hybrid migration into native RDMA migration.
- No workload-specific policy tuned only for `remap_xlarge_random_rw`.

## Migration From Current Branch

The existing `migration/cxl-rdma.c` measurement spike should be replaced, not
extended. Remove the direct-copy completion path and the shadow submit helper.
Keep only the useful accounting names after redefining them to mean real RDMA
transport work.

The existing region ownership code should be revised from exclusive RDMA
ownership to opportunistic in-flight/ready state. CXL publication must remain
allowed to win a race against RDMA.

The current Task 7 report remains historical evidence that the memcpy spike is
not an adequate experiment. Future reports must state the sidecar transport,
budget, and RDMA endpoint configuration.
