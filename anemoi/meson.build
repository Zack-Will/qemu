system_ss.add(files(
  'backend.c',
  'cache.c',
  'lm.c',
  'pool.c',
  'rmap.c',
  'runtime.c',
  'uffd.c',
))

system_ss.add(when: rdma, if_true: files('rdma.c'))
system_ss.add(when: zstd, if_true: files('replica.c'))
