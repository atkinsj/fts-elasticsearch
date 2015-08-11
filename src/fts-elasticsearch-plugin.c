/* Copyright (c) 2006-2012 Dovecot authors, see the included COPYING file */
/* Copyright (c) 2014 Joshua Atkins <josh@ascendantcom.com> */

#include "lib.h"
#include "array.h"
#include "mail-user.h"
#include "mail-storage-hooks.h"
#include "fts-elasticsearch-plugin.h"

#include <stdlib.h>

const char *fts_elasticsearch_plugin_version = DOVECOT_ABI_VERSION;

struct fts_elasticsearch_user_module fts_elasticsearch_user_module =
    MODULE_CONTEXT_INIT(&mail_user_module_register);

static int fts_elasticsearch_plugin_init_settings(struct mail_user *user,
                                        struct fts_elasticsearch_settings *set,
                                        const char *str)
{
    const char *const *tmp;

    /* validate our parameters */
    if (user == NULL || set == NULL) {
        i_error("fts_elasticsearch: critical error initialisation");
        return -1;
    }

    if (str == NULL) {
        str = "";
    }

    for (tmp = t_strsplit_spaces(str, " "); *tmp != NULL; tmp++) {
        if (strncmp(*tmp, "url=", 4) == 0) {
            set->url = p_strdup(user->pool, *tmp + 4);
        } else if (strcmp(*tmp, "debug") == 0) {
            set->debug = TRUE;
        } else {
            i_error("fts_elasticsearch: Invalid setting: %s", *tmp);
            return -1;
        }
    }

    return 0;
}

static void fts_elasticsearch_mail_user_create(struct mail_user *user,
                                               const char *env)
{
    struct fts_elasticsearch_user *fuser = NULL;

    /* validate our parameters */
    if (user == NULL || env == NULL) {
        i_error("fts_elasticsearch: critical error during mail user creation");
    } else {
        fuser = p_new(user->pool, struct fts_elasticsearch_user, 1);
        if (fts_elasticsearch_plugin_init_settings(user, &fuser->set, env) < 0) {
            /* invalid settings, disabling */
            return;
        }

        MODULE_CONTEXT_SET(user, fts_elasticsearch_user_module, fuser);
    }
}

static void fts_elasticsearch_mail_user_created(struct mail_user *user)
{
    const char *env = NULL;

    /* validate our parameters */
    if (user == NULL) {
        i_error("fts_elasticsearch: critical error during mail user creation");
    } else {
        env = mail_user_plugin_getenv(user, "fts_elasticsearch");

        if (env != NULL) {
            fts_elasticsearch_mail_user_create(user, env);
        }
    }
}

static struct mail_storage_hooks fts_elasticsearch_mail_storage_hooks = {
    .mail_user_created = fts_elasticsearch_mail_user_created
};

void fts_elasticsearch_plugin_init(struct module *module)
{
    fts_backend_register(&fts_backend_elasticsearch);
    mail_storage_hooks_add(module, &fts_elasticsearch_mail_storage_hooks);
}

void fts_elasticsearch_plugin_deinit(void)
{
    fts_backend_unregister(fts_backend_elasticsearch.name);
    mail_storage_hooks_remove(&fts_elasticsearch_mail_storage_hooks);
}

const char *fts_elasticsearch_plugin_dependencies[] = { "fts", NULL };
