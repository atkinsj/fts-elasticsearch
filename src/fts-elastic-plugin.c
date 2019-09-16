/* Copyright (c) 2006-2012 Dovecot authors, see the included COPYING file */
/* Copyright (c) 2014 Joshua Atkins <josh@ascendantcom.com> */

#include "lib.h"
#include "array.h"
#include "http-client.h"
#include "mail-user.h"
#include "mail-storage-hooks.h"
#include "fts-elastic-plugin.h"

#include <stdlib.h>

const char *fts_elastic_plugin_version = DOVECOT_ABI_VERSION;
struct http_client *elastic_http_client = NULL;

struct fts_elastic_user_module fts_elastic_user_module =
    MODULE_CONTEXT_INIT(&mail_user_module_register);

static int
fts_elastic_plugin_init_settings(struct mail_user *user,
                                 struct fts_elastic_settings *set,
                                 const char *str)
{
    const char *const *tmp;

    /* validate our parameters */
    if (user == NULL || set == NULL) {
        i_error("fts_elastic: critical error initialisation");
        return -1;
    }

    if (str == NULL) {
        str = "";
    }

    set->bulk_size = 5*1024*1024; /* 5 MB */
    set->refresh_by_fts = TRUE;
    set->refresh_on_update = FALSE;

    for (tmp = t_strsplit_spaces(str, " "); *tmp != NULL; tmp++) {
        if (strncmp(*tmp, "url=", 4) == 0) {
            set->url = p_strdup(user->pool, *tmp + 4);
        } else if (strcmp(*tmp, "debug") == 0) {
            set->debug = TRUE;
		} else if (str_begins(*tmp, "rawlog_dir=")) {
			set->rawlog_dir = p_strdup(user->pool, *tmp + 11);
		} else if (str_begins(*tmp, "bulk_size=")) {
			if (str_to_uint(*tmp+10, &set->bulk_size) < 0 || set->bulk_size == 0) {
				i_error("fts_elastic: bulk_size must be a positive integer");
                return -1;
			}
		} else if (str_begins(*tmp, "refresh=")) {
			if (strcmp(*tmp + 8, "never") == 0) {
				set->refresh_on_update = FALSE;
				set->refresh_by_fts = FALSE;
			} else if (strcmp(*tmp + 8, "update") == 0) {
				set->refresh_on_update = TRUE;
			} else if (strcmp(*tmp + 8, "fts") == 0) {
				set->refresh_by_fts = TRUE;
			} else {
				i_error("fts_elastic: Invalid setting for refresh: %s", *tmp+8);
				return -1;
			}
        } else {
            i_error("fts_elastic: Invalid setting: %s", *tmp);
            return -1;
        }
    }

    return 0;
}

static void fts_elastic_mail_user_create(struct mail_user *user, const char *env)
{
    struct fts_elastic_user *fuser = NULL;

    /* validate our parameters */
    if (user == NULL || env == NULL) {
        i_error("fts_elastic: critical error during mail user creation");
    } else {
        fuser = p_new(user->pool, struct fts_elastic_user, 1);
        if (fts_elastic_plugin_init_settings(user, &fuser->set, env) < 0) {
            /* invalid settings, disabling */
            return;
        }

        MODULE_CONTEXT_SET(user, fts_elastic_user_module, fuser);
    }
}

static void fts_elastic_mail_user_created(struct mail_user *user)
{
    const char *env = NULL;

    /* validate our parameters */
    if (user == NULL) {
        i_error("fts_elastic: critical error during mail user creation");
    } else {
        env = mail_user_plugin_getenv(user, "fts_elastic");

        if (env != NULL) {
            fts_elastic_mail_user_create(user, env);
        }
    }
}

static struct mail_storage_hooks fts_elastic_mail_storage_hooks = {
    .mail_user_created = fts_elastic_mail_user_created
};

void fts_elastic_plugin_init(struct module *module)
{
    fts_backend_register(&fts_backend_elastic);
    mail_storage_hooks_add(module, &fts_elastic_mail_storage_hooks);
}

void fts_elastic_plugin_deinit(void)
{
    fts_backend_unregister(fts_backend_elastic.name);
    mail_storage_hooks_remove(&fts_elastic_mail_storage_hooks);
    if (elastic_http_client != NULL)
		http_client_deinit(&elastic_http_client);
}

const char *fts_elastic_plugin_dependencies[] = { "fts", NULL };
