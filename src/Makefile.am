AM_CFLAGS= \
	-I../../dovecot-2.2.15/src/plugins/fts \
	$(LIBDOVECOT_INCLUDE)

AM_LDFLAGS= -ljson-c 

lib_LTLIBRARIES = lib21_fts_elasticsearch_plugin.la
lib21_fts_elasticsearch_plugin_la_SOURCES = fts-elasticsearch-plugin.c fts-backend-elasticsearch.c elasticsearch-conn.c
