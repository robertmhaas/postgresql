# contrib/pg_collect_advice/Makefile

MODULE_big = pg_collect_advice
OBJS = \
	$(WIN32RES) \
	collector.o \
	interface.o

EXTENSION = pg_collect_advice
DATA = pg_collect_advice--1.0.sql
PGFILEDESC = "pg_collect_advice - collect queries and their plan advice strings"

REGRESS = local_collector
TAP_TESTS = 1

# required for 001_regress.pl
REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_collect_advice
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
