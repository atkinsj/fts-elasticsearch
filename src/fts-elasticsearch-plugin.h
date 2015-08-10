#ifndef FTS_ELASTICSEARCH_PLUGIN_H
#define FTS_ELASTICSEARCH_PLUGIN_H

#include "module-context.h"
#include "fts-api-private.h"

#define FTS_ELASTICSEARCH_USER_CONTEXT(obj) \
    MODULE_CONTEXT(obj, fts_elasticsearch_user_module)

struct fts_elasticsearch_settings {
    bool debug;			/* whether or not debug is set */
    const char *url;	/* base URL to an ElasticSearch instance */
};

struct fts_elasticsearch_user {
    union mail_user_module_context module_ctx;	/* mail user context */
    struct fts_elasticsearch_settings set; 		/* laoded settings */
};

extern const char *fts_elasticsearch_plugin_dependencies[];
extern struct fts_backend fts_backend_elasticsearch;
extern MODULE_CONTEXT_DEFINE(fts_elasticsearch_user_module, &mail_user_module_register);

void fts_elasticsearch_plugin_init(struct module *module);
void fts_elasticsearch_plugin_deinit(void);

#endif
