#ifndef FTS_ELASTICSEARCH_PLUGIN_H
#define FTS_ELASTICSEARCH_PLUGIN_H

#include "module-context.h"
#include "fts-api-private.h"

#define FTS_ELASTICSEARCH_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, fts_elasticsearch_user_module)

struct fts_elasticsearch_settings {
	bool debug;
	const char *url;
};

struct fts_elasticsearch_user {
	union mail_user_module_context module_ctx;
	struct fts_elasticsearch_settings set;
};

extern const char *fts_elasticsearch_plugin_dependencies[];
extern struct fts_backend fts_backend_elasticsearch;
extern MODULE_CONTEXT_DEFINE(fts_elasticsearch_user_module, &mail_user_module_register);
/*extern struct http_client *elasticsearch_http_client;*/

void fts_elasticsearch_plugin_init(struct module *module);
void fts_elasticsearch_plugin_deinit(void);

#endif
