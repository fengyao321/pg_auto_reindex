# contrib/pg_auto_reindex/meson.build

pg_auto_reindex_sources = files(
  'src/pg_auto_reindex.c',
  'src/bloat_estimator.c',
  'src/executor_safe.c',
  'src/shmem_status.c',
)

if get_option('default_library') == 'shared'
  pg_auto_reindex = shared_module('pg_auto_reindex',
    pg_auto_reindex_sources,
    c_args: contrib_c_args,
    include_directories: include_directories('src'),
    dependencies: [frontend_shlib_code],
    kwargs: contrib_mod_args,
  )
  contrib_targets += pg_auto_reindex

  install_data(
    'pg_auto_reindex.control',
    'pg_auto_reindex--2.0.sql',
    'pg_auto_reindex--1.0--2.0.sql',
    kwargs: contrib_data_args,
  )
endif
