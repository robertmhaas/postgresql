# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_collect_advice_sources = files(
  'collector.c',
  'interface.c',
)

if host_system == 'windows'
  pg_collect_advice_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_collect_advice',
    '--FILEDESC', 'pg_collect_advice - collect queries and their plan advice strings',])
endif

pg_collect_advice = shared_module('pg_collect_advice',
  pg_collect_advice_sources,
  include_directories: include_directories('.'),
  kwargs: contrib_mod_args,
)
contrib_targets += pg_collect_advice

install_data(
  'pg_collect_advice--1.0.sql',
  'pg_collect_advice.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_collect_advice',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'local_collector',
    ],
  },
  'tap': {
    'tests': [
      't/001_regress.pl',
    ],
  },
}
