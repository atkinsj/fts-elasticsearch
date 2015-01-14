AM_CFLAGS= \
    -I$(dovecotdir)/src/plugins/fts \
	$(LIBDOVECOT_INCLUDE) 

AM_LDFLAGS= -ljson-c 

lib21_fts_elasticsearch_plugin_la_LDFLAGS = -module -avoid-version

lib21_fts_elasticsearch_plugin_ladir = $(dovecot_pkgincludedir)
lib21_fts_elasticsearch_plugin_la_LTLIBRARIES = lib21_fts_elasticsearch_plugin.la
lib21_fts_elasticsearch_plugin_la_SOURCES = fts-elasticsearch-plugin.c fts-backend-elasticsearch.c elasticsearch-conn.c
