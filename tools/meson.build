if have_tools and rdma.found()
  executable('anemoi-pool',
             files('anemoi-pool.c',
                   '../anemoi/backend.c',
                   '../anemoi/pool.c',
                   '../anemoi/rdma.c'),
             dependencies: [qemuutil, rdma],
             install: true)
endif
