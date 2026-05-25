; Minimal x86 boot sector workload for hybrid postcopy warm experiments.
; It switches to 32-bit protected mode and repeatedly touches one byte in each
; 4K page across a configurable physical range, creating a stable hot working
; set with strong spatial locality.

BITS 16
ORG 0x7c00

%ifndef PRESSURE_OUTER_SPIN
%define PRESSURE_OUTER_SPIN 1
%endif
%ifndef PRESSURE_START_ADDR
%define PRESSURE_START_ADDR 0x00020000
%endif
%ifndef PRESSURE_END_ADDR
%define PRESSURE_END_ADDR 0x000a0000
%endif
%ifndef PRESSURE_WRITES_PER_PAGE
%define PRESSURE_WRITES_PER_PAGE 2
%endif
%ifndef PRESSURE_PAGE_ORDER_RANDOM
%define PRESSURE_PAGE_ORDER_RANDOM 0
%endif
%ifndef PRESSURE_ACCESS_PATTERN_RANDOM_RW
%define PRESSURE_ACCESS_PATTERN_RANDOM_RW 0
%endif
%ifndef PRESSURE_ACCESS_PATTERN_READ_ONLY
%define PRESSURE_ACCESS_PATTERN_READ_ONLY 0
%endif
%ifndef PRESSURE_RANDOM_PAGE_STRIDE
%define PRESSURE_RANDOM_PAGE_STRIDE 73
%endif
%ifndef PRESSURE_RANDOM_EPOCH_SEED_STEP
%define PRESSURE_RANDOM_EPOCH_SEED_STEP 1
%endif
%ifndef PRESSURE_SAMPLE_INTERVAL_PAGES
%define PRESSURE_SAMPLE_INTERVAL_PAGES 0
%endif
%ifndef PRESSURE_IN_MEMORY_LATENCY
%define PRESSURE_IN_MEMORY_LATENCY 0
%endif
%ifndef PRESSURE_IN_MEMORY_LOG_ADDR
%define PRESSURE_IN_MEMORY_LOG_ADDR 0x02000000
%endif
%ifndef PRESSURE_IN_MEMORY_LOG_RECORDS
%define PRESSURE_IN_MEMORY_LOG_RECORDS 4194304
%endif
%ifndef PRESSURE_IN_MEMORY_MARKER_ADDR
%define PRESSURE_IN_MEMORY_MARKER_ADDR 0x02800000
%endif

%define PAGE_SIZE_SHIFT 12
%define PAGE_COUNT ((PRESSURE_END_ADDR - PRESSURE_START_ADDR) >> PAGE_SIZE_SHIFT)
%define PAGE_MASK (PAGE_COUNT - 1)
%define IN_MEMORY_LOG_MAGIC 0x4d4c5843
%define IN_MEMORY_LOG_VERSION 1
%define IN_MEMORY_LOG_HEADER_BYTES 32
%define IN_MEMORY_MARKER_MAGIC 0x4d4b5843
%define IN_MEMORY_MARKER_VERSION 1
%define IN_MEMORY_MARKER_HEADER_BYTES 32

%if PRESSURE_PAGE_ORDER_RANDOM
%if (PAGE_COUNT & PAGE_MASK) != 0
%error "random page order requires a power-of-two page count"
%endif
%endif
%if PRESSURE_SAMPLE_INTERVAL_PAGES
%if (PRESSURE_SAMPLE_INTERVAL_PAGES & (PRESSURE_SAMPLE_INTERVAL_PAGES - 1)) != 0
%error "sample interval requires a power-of-two page count"
%endif
%endif
%if PRESSURE_IN_MEMORY_LATENCY && !PRESSURE_SAMPLE_INTERVAL_PAGES
%error "in-memory latency logging requires sample interval"
%endif

%macro ACCESS_PAGE 0
%if PRESSURE_ACCESS_PATTERN_READ_ONLY
    mov al, [ebx]
    mov al, [ebx + 1]
%elif PRESSURE_ACCESS_PATTERN_RANDOM_RW
    test edi, 1
    jnz %%do_write
    mov al, [ebx]
    mov al, [ebx + 1]
    jmp %%done
%%do_write:
    mov al, [ebx]
    add al, 1
    mov [ebx], al
    mov [ebx + 1], al
%%done:
    inc edi
%else
    mov ecx, PRESSURE_WRITES_PER_PAGE
%%page_writes:
    mov al, [ebx]
    add al, 1
    mov [ebx], al
    mov [ebx + 1], al
    dec ecx
    jnz %%page_writes
%endif
%endmacro

%macro INIT_IN_MEMORY_LOG 0
%if PRESSURE_IN_MEMORY_LATENCY
    mov dword [PRESSURE_IN_MEMORY_LOG_ADDR + 0], IN_MEMORY_LOG_MAGIC
    mov dword [PRESSURE_IN_MEMORY_LOG_ADDR + 4], IN_MEMORY_LOG_VERSION
    mov dword [PRESSURE_IN_MEMORY_LOG_ADDR + 8], PRESSURE_SAMPLE_INTERVAL_PAGES
    mov dword [PRESSURE_IN_MEMORY_LOG_ADDR + 12], PRESSURE_IN_MEMORY_LOG_RECORDS
    mov dword [PRESSURE_IN_MEMORY_LOG_ADDR + 16], 0
    mov dword [PRESSURE_IN_MEMORY_LOG_ADDR + 20], 0
    rdtsc
    mov [PRESSURE_IN_MEMORY_LOG_ADDR + 24], eax
    mov [PRESSURE_IN_MEMORY_LOG_ADDR + 28], edx
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 0], IN_MEMORY_MARKER_MAGIC
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 4], IN_MEMORY_MARKER_VERSION
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 8], 0
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 12], PRESSURE_IN_MEMORY_LOG_RECORDS
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 16], 0
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 20], 0
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 24], 0
    mov dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 28], 0
%endif
%endmacro

%macro SAMPLE_LATENCY 0
%if PRESSURE_IN_MEMORY_LATENCY
    push eax
    push edx
    push ecx
    push ebp
    rdtsc
    mov ecx, eax
    mov ebp, edx
    sub eax, [PRESSURE_IN_MEMORY_LOG_ADDR + 24]
    mov edx, [PRESSURE_IN_MEMORY_LOG_ADDR + 16]
    inc dword [PRESSURE_IN_MEMORY_LOG_ADDR + 16]
    and edx, PRESSURE_IN_MEMORY_LOG_RECORDS - 1
    mov [PRESSURE_IN_MEMORY_LOG_ADDR + IN_MEMORY_LOG_HEADER_BYTES + edx * 4], eax
    mov eax, [PRESSURE_IN_MEMORY_MARKER_ADDR + 16]
    mov [PRESSURE_IN_MEMORY_MARKER_ADDR + IN_MEMORY_MARKER_HEADER_BYTES + edx * 4], eax
    inc dword [PRESSURE_IN_MEMORY_MARKER_ADDR + 8]
    mov edx, [PRESSURE_IN_MEMORY_LOG_ADDR + 16]
    cmp edx, PRESSURE_IN_MEMORY_LOG_RECORDS
    jbe %%store_last
    mov edx, [PRESSURE_IN_MEMORY_LOG_ADDR + 20]
    inc edx
    mov [PRESSURE_IN_MEMORY_LOG_ADDR + 20], edx
%%store_last:
    mov [PRESSURE_IN_MEMORY_LOG_ADDR + 24], ecx
    mov [PRESSURE_IN_MEMORY_LOG_ADDR + 28], ebp
    pop ebp
    pop ecx
    pop edx
    pop eax
%else
    push edx
    mov dx, 0x00e9
    mov al, '+'
    out dx, al
    pop edx
%endif
%endmacro

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00
    call enable_a20
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_start

enable_a20:
    in al, 0x92
    or al, 0x02
    out 0x92, al
    ret

gdt_start:
    dq 0x0000000000000000
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BITS 32
protected_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x0009f000
    INIT_IN_MEMORY_LOG
%if PRESSURE_PAGE_ORDER_RANDOM || PRESSURE_ACCESS_PATTERN_RANDOM_RW
    xor edi, edi
%endif
%if PRESSURE_PAGE_ORDER_RANDOM
    xor esi, esi
%endif

.outer:
%if PRESSURE_PAGE_ORDER_RANDOM
    mov ebp, esi
    mov edx, PAGE_COUNT

.inner:
    mov ebx, ebp
    shl ebx, PAGE_SIZE_SHIFT
    add ebx, PRESSURE_START_ADDR
    ACCESS_PAGE
    add ebp, PRESSURE_RANDOM_PAGE_STRIDE
    and ebp, PAGE_MASK
    dec edx
%if PRESSURE_SAMPLE_INTERVAL_PAGES
%if PRESSURE_IN_MEMORY_LATENCY
    test edx, PRESSURE_SAMPLE_INTERVAL_PAGES - 1
    jnz .inner
    SAMPLE_LATENCY
    test edx, edx
    jz .inner_done
    jmp .inner
.inner_done:
%else
    jz .inner_done
    test edx, PRESSURE_SAMPLE_INTERVAL_PAGES - 1
    jnz .inner
    SAMPLE_LATENCY
    jmp .inner
.inner_done:
%endif
%else
    jnz .inner
%endif
    add esi, PRESSURE_RANDOM_EPOCH_SEED_STEP
    and esi, PAGE_MASK
%else
%if PRESSURE_SAMPLE_INTERVAL_PAGES
    xor edi, edi
%endif
    mov ebx, PRESSURE_START_ADDR

.inner:
    ACCESS_PAGE
    add ebx, 0x1000
%if PRESSURE_SAMPLE_INTERVAL_PAGES
    inc edi
    test edi, PRESSURE_SAMPLE_INTERVAL_PAGES - 1
    jnz .no_sample
    SAMPLE_LATENCY
.no_sample:
%endif
    cmp ebx, PRESSURE_END_ADDR
    jb .inner
%endif
%if PRESSURE_OUTER_SPIN > 0
    mov ecx, PRESSURE_OUTER_SPIN
.spin:
    loop .spin
%endif
%if !PRESSURE_IN_MEMORY_LATENCY
    mov dx, 0x00e9
    mov al, '.'
    out dx, al
%endif
    jmp .outer

times 510 - ($ - $$) db 0
dw 0xaa55
