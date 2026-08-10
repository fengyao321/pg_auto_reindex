# contrib/pg_auto_reindex/Makefile

MODULE_big = pg_auto_reindex
OBJS = \
	$(WIN32RES) \
	pg_auto_reindex.o \
	idle_learner.o \
	bloat_estimator.o \
	reindex_executor.o

EXTENSION = pg_auto_reindex
DATA = pg_auto_reindex--1.0.sql
PGFILEDESC = "pg_auto_reindex - autonomous idle learning & background concurrent reindexing"
REGRESS = pg_auto_reindex

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_auto_reindex
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
