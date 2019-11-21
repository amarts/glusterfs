xlator_LTLIBRARIES = dsmds.la
xlatordir = $(libdir)/glusterfs/$(PACKAGE_VERSION)/xlator/features

dsmds_la_LDFLAGS = -module $(GF_XLATOR_DEFAULT_LDFLAGS)

dsmds_la_SOURCES = dsmds.c
dsmds_la_LIBADD = $(top_builddir)/libglusterfs/src/libglusterfs.la

noinst_HEADERS = dsmds.h

AM_CPPFLAGS = $(GF_CPPFLAGS) -I$(top_srcdir)/libglusterfs/src \
	-I$(top_srcdir)/rpc/xdr/src -I$(top_builddir)/rpc/xdr/src \
	-I$(top_srcdir)/xlators/lib/src

AM_CFLAGS = -Wall $(GF_CFLAGS)

CLEANFILES =
