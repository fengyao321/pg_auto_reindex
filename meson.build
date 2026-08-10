# contrib/pg_auto_reindex/meson.build

pg_auto_reindex_sources = files(
  'pg_auto_reindex.c',
  'idle_learner.c',
  'bloat_estimator.c',
  'reindex_executor.c',
)

if get_option('default_library') == 'shared'
  pg_auto_reindex = shared_module('pg_auto_reindex',
    pg_auto_reindex_sources,
    c_args: contrib_c_args,
    dependencies: [frontend_shlib_code],
    kwargs: contrib_mod_args,
  )
  contrib_targets += pg_auto_reindex

  install_data(
    'pg_auto_reindex.control',
    'pg_auto_reindex--1.0.sql',
    kwargs: contrib_data_args,
  )
endif
