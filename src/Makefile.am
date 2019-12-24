AM_CFLAGS= \
    -I$(abs_dovecotdir)/src/plugins/fts \
	$(LIBDOVECOT_INCLUDE) 

AM_LDFLAGS= -ljson-c 

lib21_fts_elastic_plugin_la_LDFLAGS = -module -avoid-version

lib21_fts_elastic_plugin_ladir = $(dovecot_pkglibdir)
lib21_fts_elastic_plugin_la_LTLIBRARIES = lib21_fts_elastic_plugin.la
lib21_fts_elastic_plugin_la_SOURCES = fts-elastic-plugin.c fts-backend-elastic.c elastic-connection.c
