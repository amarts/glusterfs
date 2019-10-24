if WITH_SERVER
xlator_LTLIBRARIES = prjquota.la
endif
xlatordir = $(libdir)/glusterfs/$(PACKAGE_VERSION)/xlator/features

prjquota_la_LDFLAGS = -module $(GF_XLATOR_DEFAULT_LDFLAGS)

prjquota_la_SOURCES = prjquota.c
prjquota_la_LIBADD = $(top_builddir)/libglusterfs/src/libglusterfs.la

noinst_HEADERS = prjquota.h

AM_CPPFLAGS = $(GF_CPPFLAGS) -I$(top_srcdir)/libglusterfs/src

AM_CFLAGS = -Wall -fno-strict-aliasing $(GF_CFLAGS)

CLEANFILES =
