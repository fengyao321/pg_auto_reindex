# contrib/pg_auto_reindex/Makefile

MODULE_big = pg_auto_reindex
OBJS = \
	$(WIN32RES) \
	src/pg_auto_reindex.o \
	src/bloat_estimator.o \
	src/executor_safe.o \
	src/shmem_status.o

EXTENSION = pg_auto_reindex
DATA = pg_auto_reindex--2.0.sql pg_auto_reindex--1.0--2.0.sql
PGFILEDESC = "pg_auto_reindex - autonomous B-Tree bloat estimation and safe concurrent reindexing"
REGRESS = pg_auto_reindex

PG_CPPFLAGS = -I$(srcdir)/src

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
