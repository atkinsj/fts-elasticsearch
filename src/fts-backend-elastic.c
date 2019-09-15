#include <ctype.h>
#include <syslog.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>

#include "lib.h"
#include "array.h"
#include "str.h"
#include "hash.h"
#include "strescape.h"
#include "unichar.h"
#include "mail-storage-private.h"
#include "mailbox-list-private.h"
#include "mail-search.h"
#include "fts-api.h"
#include "fts-elastic-plugin.h"
#include "elastic-connection.h"

/* values that must be escaped in query fields */
static const char *es_query_escape_chars = "\"\\";

/* values that must be escaped in a bulk update value field */
static const char *es_update_escape_chars = "\"\\";

/* values that must be escaped in field names */
static const char *es_field_escape_chars = ".#*\"";

/* the search JSON */
static const char JSON_BOOL_MULTI_MATCH[] = 
    "\"%s\":{"
        "\"multi_match\":{"
            "\"query\":\"%s\","
            "\"operator\":\"%s\","
            "\"fields\":[%s]"
        "}"
    "}";

/* the last_uid lookup json */
static const char JSON_LAST_UID[] =
    "{"
      "\"sort\":{"
        "\"uid\":\"desc\""
      "},"
      "\"query\":{"
        "\"match_all\":{}"
      "},"
      "\"size\":1"
    "}";

/* bulk index header */
static const char JSON_BULK_HEADER[] =
    "{"
      "\"%s\":{"
        "\"_index\":\"box-%s\","
        "\"_id\":%d"
      "}"
    "}"
    "\n";

struct elastic_fts_backend {
    struct fts_backend backend;
    struct elastic_connection *elastic_conn;
};

struct elastic_fts_field {
	char *key;
	string_t *value;
};

struct elastic_fts_backend_update_context {
    struct fts_backend_update_context ctx;

    struct mailbox *prev_box;
    char box_guid[MAILBOX_GUID_HEX_LENGTH + 1];
    
    uint32_t prev_uid;

    /* used to build multi-part messages. */
    string_t *current_key;
    string_t *current_value;

	ARRAY(struct elastic_fts_field) fields;

    /* build a json string for bulk indexing */
    string_t *json_request;

    /* current request size */
    size_t request_size;

    unsigned int body_open:1;
    unsigned int documents_added:1;
    unsigned int expunges:1;
};

static const char *es_replace(const char *str, const char *replace)
{
    string_t *ret;
    uint32_t i;

    ret = t_str_new(strlen(str) + 16);

    for (i = 0; str[i] != '\0'; i++) {
        if (strchr(replace, str[i]) != NULL)
            str_append_c(ret, '_');
        else
            str_append_c(ret, str[i]);
    }

    return str_c(ret);
}

static const char *es_escape(const char *str, const char *escape)
{
    string_t *ret;
    uint32_t i;

    ret = t_str_new(strlen(str) + 16);

    for (i = 0; str[i] != '\0'; i++) {
        if (strchr(escape, str[i]) != NULL)
            str_append_c(ret, '\\');

        /* escape control characters that JSON isn't a fan of */
        switch(str[i])
        {
            case '\t': str_append(ret, "\\t"); break;
            case '\b': str_append(ret, "\\b"); break;
            case '\n': str_append(ret, "\\n"); break;
            case '\r': str_append(ret, "\\r"); break;
            case '\f': str_append(ret, "\\f"); break;
            case 0x1C: str_append(ret, "0x1C"); break;
            case 0x1D: str_append(ret, "0x1D"); break;
            case 0x1E: str_append(ret, "0x1E"); break;
            case 0x1F: str_append(ret, "0x1F"); break;
            default: str_append_c(ret, str[i]); break;
        }
    }

    return str_c(ret);
}

static const char *es_update_escape(const char *str)
{
    return es_escape(str, es_update_escape_chars);
}

static const char *es_query_escape(const char *str)
{
    return es_escape(str, es_query_escape_chars);
}

static void str_append_message_id(string_t *str, uint32_t uid, 
                                  struct elastic_fts_backend_update_context *ctx)
{
	str_printfa(str, "%u/%s", uid, ctx->box_guid);
	if (ctx->ctx.backend->ns->owner != NULL) {
		str_printfa(str, "/%s", ctx->ctx.backend->ns->owner->username);
	}
}

static struct fts_backend *fts_backend_elastic_alloc(void)
{
    struct elastic_fts_backend *backend;

    backend = i_new(struct elastic_fts_backend, 1);
    backend->backend = fts_backend_elastic;

    return &backend->backend;
}

static int
fts_backend_elastic_init(struct fts_backend *_backend,
                         const char **error_r ATTR_UNUSED)
{
    struct elastic_fts_backend *backend = NULL;
    struct fts_elastic_user *fuser = NULL;

    /* ensure our backend is provided */
    if (_backend != NULL) {
        backend = (struct elastic_fts_backend *)_backend;
        fuser = fts_elastic_USER_CONTEXT(_backend->ns->user);
    } else {
        *error_r = "fts_elastic: error during backend initialisation";
        return -1;
    }

    if (fuser == NULL) {
        *error_r = "Invalid fts_elastic setting";
        return -1;
    }

    return elastic_connection_init(&fuser->set, &backend->elastic_conn, error_r);
}

static void
fts_backend_elastic_deinit(struct fts_backend *_backend)
{
    i_free(_backend);
}

static void
fts_backend_elastic_bulk_end(struct elastic_fts_backend_update_context *_ctx)
{
    struct elastic_fts_backend_update_context *ctx = NULL;
	struct elastic_fts_field *field;

    /* ensure we have a context */
    if (_ctx != NULL) {
        ctx = (struct elastic_fts_backend_update_context *)_ctx;

        array_foreach_modifiable(&ctx->fields, field) {
            str_printfa(ctx->json_request,
                        ", \"%s\": \"%s\"",
                        es_replace(field->key, es_field_escape_chars),
                        es_update_escape(str_c(field->value))
                        );
            /* clean */
            str_free(&field->value);
            i_free(field->key);
        }
    	array_clear(&ctx->fields);

        /* close up this line in the bulk request */
        str_append(ctx->json_request, "}\n");

        /* clean-up for the next message */
        str_truncate(ctx->current_key, 0);
        str_truncate(ctx->current_value, 0);

        if (ctx->body_open) {
            ctx->body_open = FALSE;
        }
    }
}

static int
fts_backend_elastic_get_last_uid(struct fts_backend *_backend,
                                 struct mailbox *box,
                                 uint32_t *last_uid_r)
{
    struct fts_index_header hdr;
    struct elastic_fts_backend *backend = NULL;
    const char *box_guid = NULL;
    string_t *cmd = t_str_new_const(JSON_LAST_UID, sizeof(JSON_LAST_UID));
    int32_t ret;

    /* ensure our backend has been initialised */
    if (_backend == NULL || box == NULL || last_uid_r == NULL) {
        i_error("fts_elastic: critical error in get_last_uid");

        return -1;
    } else {
        /* keep track of our backend */
        backend = (struct elastic_fts_backend *)_backend;
    }

    /**
     * assume the dovecot index will always match ours for uids. this saves
     * on repeated calls to ES, particularly noticable when fts_autoindex=true.
     *
     * this has a couple of side effects:
     *  1. if the ES index has been blown away, this will return a valid
     *     last_uid that matches Dovecot and it won't realise we need updating
     *  2. if data has been indexed by Dovecot but missed by ES (outage, etc)
     *     then it won't ever make it to the ES index either.
     *
     * TODO: find a better way to implement this
     **/
    if (fts_index_get_header(box, &hdr)) {
        *last_uid_r = hdr.last_indexed_uid;
        return 0;
    } 

    if (fts_mailbox_get_guid(box, &box_guid) < 0) {
        i_error("fts_elastic: get_last_uid: failed to get mbox guid");
        return -1;
    }

    /* call ES */
    ret = elastic_connection_last_uid(backend->elastic_conn, cmd, box_guid);

    if (ret > 0) {
        *last_uid_r = ret;
        fts_index_set_last_uid(box, *last_uid_r);
        return 0;
    }
    
    *last_uid_r = 0;
    fts_index_set_last_uid(box, *last_uid_r);
    return 0;
}

static struct fts_backend_update_context *
fts_backend_elastic_update_init(struct fts_backend *_backend)
{
    struct elastic_fts_backend_update_context *ctx = NULL;

    ctx = i_new(struct elastic_fts_backend_update_context, 1);
    ctx->ctx.backend = _backend;

    /* allocate strings for building messages and multi-part messages
     * with a sensible initial size. */
    ctx->current_key = str_new(default_pool, 64);
    ctx->current_value = str_new(default_pool, 1024 * 64);
    ctx->json_request = str_new(default_pool, 1024 * 64);
    ctx->request_size = 0;
	i_array_init(&ctx->fields, 16);

    return &ctx->ctx;
}

static int
fts_backend_elastic_update_deinit(struct fts_backend_update_context *_ctx)
{
    struct elastic_fts_backend_update_context *ctx = NULL;
    struct elastic_fts_backend *backend = NULL;
	struct elastic_fts_field *field;

    /* validate our input parameters */
    if (_ctx == NULL || _ctx->backend == NULL) {
        i_error("fts_elastic: critical error in update_deinit");

        return -1;
    } else {
        ctx = (struct elastic_fts_backend_update_context *)_ctx;
        backend = (struct elastic_fts_backend *)_ctx->backend;
    }

    /* clean-up: expunges don't need as much clean-up */
    if (!ctx->expunges) {
        /* this gets called when the last message is finished, so close it up */
        fts_backend_elastic_bulk_end(ctx);

        /* cleanup */
        memset(ctx->box_guid, 0, sizeof(ctx->box_guid));
        str_free(&ctx->current_key);
        str_free(&ctx->current_value);
        ctx->request_size = 0;
        array_foreach_modifiable(&ctx->fields, field) {
            str_free(&field->value);
            i_free(field->key);
        }
    	array_free(&ctx->fields);
    }

    /* perform the actual post */
    elastic_connection_update(backend->elastic_conn, ctx->json_request);

    /* global clean-up */
    str_free(&ctx->json_request); 
    i_free(ctx);
    
    return 0;
}

static void
fts_backend_elastic_update_set_mailbox(struct fts_backend_update_context *_ctx,
                                       struct mailbox *box)
{
    struct elastic_fts_backend_update_context *ctx = NULL;
    const char *box_guid = NULL;

    if (_ctx != NULL) {
        ctx = (struct elastic_fts_backend_update_context *)_ctx;

        /* update_set_mailbox has been called but the previous uid is not 0;
         * clean up from our previous mailbox indexing. */
        if (ctx->prev_uid != 0) {
            fts_index_set_last_uid(ctx->prev_box, ctx->prev_uid);

            ctx->prev_uid = 0;
        }

        if (box != NULL) {
            if (fts_mailbox_get_guid(box, &box_guid) < 0) {
                i_debug("fts_elastic: update_set_mailbox: fts_mailbox_get_guid failed");

                _ctx->failed = TRUE;
            }

            /* store the current mailbox we're on in our state struct */
            i_assert(strlen(box_guid) == sizeof(ctx->box_guid) - 1);
            memcpy(ctx->box_guid, box_guid, sizeof(ctx->box_guid) - 1);
        } else {
            /* a box of null appears to indicate that indexing is complete. */
            memset(ctx->box_guid, 0, sizeof(ctx->box_guid));
        }

        ctx->prev_box = box;
    } else {
        i_error("fts_elastic: update_set_mailbox: context was NULL");

        return;
    }
}

static void
elastic_add_update_field(struct elastic_fts_backend_update_context *ctx,
                         string_t *key, string_t *value)
{
	struct elastic_fts_field *field;
	struct elastic_fts_field new_field;

	/* there are only a few fields. this lookup is fast enough. */
	array_foreach(&ctx->fields, field) {
		if (strcasecmp(field->key, str_c(key)) == 0) {
			str_append_str(field->value, value);
            return;
        }
	}

	i_zero(&new_field);
	new_field.key = str_lcase(i_strdup(str_c(key)));
	new_field.value = str_new(default_pool, 128);
	array_push_back(&ctx->fields, &new_field);
    str_append_str(new_field.value, value);
    return;
}

static void
fts_backend_elastic_bulk_start(struct elastic_fts_backend_update_context *_ctx,
                               uint32_t uid, string_t *json_request,
                               const char *action_name)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;

    /* track that we've added documents */
    ctx->documents_added = TRUE;

    /* add the header that starts the bulk transaction */
    str_printfa(json_request, JSON_BULK_HEADER, action_name, ctx->box_guid, uid);

    /* expunges don't need anything more than the action line */
    if (!ctx->expunges) {
        /* add the first two fields; these are static on every message. */
        /* TODO: add user to fields, when there will be ability to index messages
         * from more users in one index*/
        str_printfa(json_request, "{\"uid\":%d,\"box\":\"%s\"", uid, ctx->box_guid);
    }
}

static void
fts_backend_elastic_uid_changed(struct fts_backend_update_context *_ctx,
                                uint32_t uid)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;
    struct elastic_fts_backend *backend =
        (struct elastic_fts_backend *)_ctx->backend;
	struct fts_elastic_user *fuser =
        fts_elastic_USER_CONTEXT(_ctx->backend->ns->user);

    if (ctx->documents_added) {
        /* this is the end of an old message. nb: the last message to be indexed
         * will not reach here but will instead be caught in update_deinit. */
        fts_backend_elastic_bulk_end(ctx);
    }

    /* chunk up our requests in to reasonable sizes */
    if (ctx->request_size > fuser->set.bulk_size) {  
        /* do an early post */
        elastic_connection_update(backend->elastic_conn, ctx->json_request);

        /* reset our tracking variables */
        str_truncate(ctx->json_request, 0);
        ctx->request_size = 0;
    }
    
    ctx->prev_uid = uid;
    
    fts_backend_elastic_bulk_start(ctx, uid, ctx->json_request, "index");
}

static const char *wanted_headers[] = {
	"From",
    "To",
    "Cc",
    "Bcc",
    "Subject",
    "Sender",
    "Message-ID",
};

static bool
fts_backend_elastic_header_want(const char *hdr_name)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(wanted_headers); i++) {
		if (strcasecmp(hdr_name, wanted_headers[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

static bool
fts_backend_elastic_update_set_build_key(struct fts_backend_update_context *_ctx,
                                         const struct fts_backend_build_key *key)
{
    struct elastic_fts_backend_update_context *ctx = NULL;

    /* validate our input */
    if (_ctx == NULL || key == NULL) {
        return FALSE;
    } else {
        ctx = (struct elastic_fts_backend_update_context *)_ctx;
    }

    /* if the uid doesn't match our expected one, we've moved on to a new message */
    if (key->uid != ctx->prev_uid) {
        fts_backend_elastic_uid_changed(_ctx, key->uid);
    }

    switch (key->type) {
    case FTS_BACKEND_BUILD_KEY_HDR:
    case FTS_BACKEND_BUILD_KEY_MIME_HDR:
        /* Index only wanted headers */
        if (fts_backend_elastic_header_want(key->hdr_name)){
            str_printfa(ctx->current_key, "%s", t_str_lcase(key->hdr_name));
        }
        break;
    case FTS_BACKEND_BUILD_KEY_BODY_PART:
        if (!ctx->body_open) {
            ctx->body_open = TRUE;
            str_append(ctx->current_key, "body");
        }

        break;
    case FTS_BACKEND_BUILD_KEY_BODY_PART_BINARY:
        i_unreached();
    }

    return TRUE;
}

static int
fts_backend_elastic_update_build_more(struct fts_backend_update_context *_ctx,
                                      const unsigned char *data, size_t size)
{
    struct elastic_fts_backend_update_context *ctx;

    if (_ctx != NULL) {
        ctx = (struct elastic_fts_backend_update_context *)_ctx;

        /* build more message body */
        str_append_max(ctx->current_value, (const char *) data, size);

        /* keep track of the total request size for chunking */
        ctx->request_size += size;

        return 0;
    } else {
        i_error("fts_elastic: update_build_more: critical error building message body");

        return -1;
    }
}

static void
fts_backend_elastic_update_unset_build_key(struct fts_backend_update_context *_ctx)
{
    struct elastic_fts_backend_update_context *ctx = NULL;

    if (_ctx != NULL) {
        ctx = (struct elastic_fts_backend_update_context *)_ctx;

        /* field is complete, add it to our message if not empty. */
        if (strcmp(str_c(ctx->current_key), "") != 0) {
            elastic_add_update_field(ctx, ctx->current_key, ctx->current_value);
        }

        /* clean-up our temp */
        str_truncate(ctx->current_key, 0);
        str_truncate(ctx->current_value, 0);
    }
}

static void
fts_backend_elastic_update_expunge(struct fts_backend_update_context *_ctx,
                                   uint32_t uid)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;

    /* update the context to note that there have been expunges */
    ctx->expunges = TRUE;

    /* add the delete action */
    fts_backend_elastic_bulk_start(ctx, uid, ctx->json_request, "delete");
}

static int fts_backend_elastic_refresh(struct fts_backend *_backend)
{
    struct elastic_fts_backend *backend =
        (struct elastic_fts_backend *)_backend;
	struct fts_elastic_user *fuser =
        fts_elastic_USER_CONTEXT(_backend->ns->user);

    if (fuser->set.refresh_by_fts) {
        elastic_connection_refresh(backend->elastic_conn);
    }
    return 0;
}

static int fts_backend_elastic_rescan(struct fts_backend *backend ATTR_UNUSED)
{    
    return fts_backend_reset_last_uids(backend);
}

static int fts_backend_elastic_optimize(struct fts_backend *backend ATTR_UNUSED)
{
    return 0;
}

static bool
elastic_add_definite_query(string_t *_fields, string_t *_fields_not,
                           string_t *value, struct mail_search_arg *arg)
{
    string_t *fields = NULL;

    /* validate our input */
    if (_fields == NULL || _fields_not == NULL || value == NULL || arg == NULL) {
        i_error("fts_elastic: critical error while building query");

        return FALSE;
    }

    if (arg->match_not) {
        fields = _fields_not;
    } else {
        fields = _fields;
    }

    switch (arg->type) {
    case SEARCH_TEXT:
        /* we don't actually have to do anything here; leaving the fields
         * array blank is sufficient to cause full text search with ES */

        break;
    case SEARCH_BODY:
        /* SEARCH_BODY has a hdr_field_name of null. we append a comma here 
         * because body can be selected in addition to other fields. it's 
         * trimmed later before being passed to ES if it's the last element. */
        str_append(fields, "\"body\",");

        break;
    case SEARCH_HEADER: /* fall through */
    case SEARCH_HEADER_ADDRESS: /* fall through */
    case SEARCH_HEADER_COMPRESS_LWSP:
        if (!fts_header_want_indexed(arg->hdr_field_name)) {
            i_debug("fts_elastic: field %s was skipped", arg->hdr_field_name);

            return FALSE;
        }

        str_append(fields, "\"");
        str_append(fields, t_str_lcase(es_query_escape(arg->hdr_field_name)));
        str_append(fields, "\",");

        break;
    default:
        return FALSE;
    }

    if (arg->match_not) {
        i_debug("fts_elastic: arg->match_not is true");
    }
    

    return TRUE;
}

static bool
elastic_add_definite_query_args(string_t *fields, string_t *fields_not,
                                string_t *value, struct mail_search_arg *arg)
{
    bool field_added = FALSE;

    if (fields == NULL || value == NULL || arg == NULL) {
        i_error("fts_elastic: critical error while building query");

        return FALSE;
    }

    for (; arg != NULL; arg = arg->next) {
        /* multiple fields have an initial arg of nothing useful and subargs */
        if (arg->value.subargs != NULL) {
            field_added = elastic_add_definite_query_args(fields, fields_not, value,
                arg->value.subargs);
        }

        if (elastic_add_definite_query(fields, fields_not, value, arg)) {
            /* the value is the same for every arg passed, only add the value
             * to our search json once. */
            if (!field_added) {
                /* we always want to add the value */
                str_append(value, es_query_escape(arg->value.str));
            }

            /* this is important to set. if this is FALSE, Dovecot will fail
             * over to its regular built-in search to produce results for
             * this argument. */
            arg->match_always = TRUE;
            field_added = TRUE;
        }
    }

    return field_added;
}

static int
fts_backend_elastic_lookup(struct fts_backend *_backend, struct mailbox *box,
                           struct mail_search_arg *args,
                           enum fts_lookup_flags flags,
                           struct fts_result *result)
{
    /* state tracking */
    struct elastic_fts_backend *backend = NULL;
    struct elastic_result **es_results = NULL;
    bool and_args = (flags & FTS_LOOKUP_FLAG_AND_ARGS) != 0;

    /* mailbox information */
    struct mailbox_status status;
    const char *box_guid = NULL;

    /* temp variables */
    pool_t pool = pool_alloconly_create("fts elastic search", 1024);
    int32_t ret = -1;
    size_t num_rows = 0;
    /* json query building */
    string_t *str = str_new(pool, 1024);
    string_t *query = str_new(pool, 1024);
    string_t *fields = str_new(pool, 1024);
    string_t *fields_not = str_new(pool, 1024);

    /* validate our input */
    if (_backend == NULL || box == NULL || args == NULL || result == NULL) {
        i_error("fts_elastic: critical error during lookup");

        return -1;
    }

    backend = (struct elastic_fts_backend *)_backend;
    

    /* get the mailbox guid */
    if (fts_mailbox_get_guid(box, &box_guid) < 0) {
        return -1;
    }

    /* open the mailbox */
    mailbox_get_open_status(box, STATUS_UIDNEXT, &status);

    /* default ES is limited to 10,000 results */
    /* TODO: paginate? */
    if (status.uidnext >= 10000) {
        num_rows = 10000;
    } else {
        num_rows = status.uidnext;
    }

    /* attempt to build the query */
    if (!elastic_add_definite_query_args(fields, fields_not, query, args)) {
        return -1;
    }

    /* remove the trailing ',' */
    str_delete(fields, str_len(fields) - 1, 1);
    str_delete(fields_not, str_len(fields_not) - 1, 1);

    /* if no fields were added, add some sensible default fields */
    if (str_len(fields) == 0 && str_len(fields_not) == 0) {
        str_append(fields, "\"from\",\"to\",\"cc\",\"bcc\",\"sender\",\"subject\",\"body\"");
    }

    /* generate json search query */
    str_append(str, "{\"query\": {\"bool\": {");
    if (str_len(fields) > 0) {
        str_printfa(str, JSON_BOOL_MULTI_MATCH, "must", str_c(query), and_args ? "and" : "or", str_c(fields));
        if (str_len(fields_not) > 0) {
            str_append(str, ",");
        }
    }
    if (str_len(fields_not) > 0) {
        str_printfa(str, JSON_BOOL_MULTI_MATCH, "must_not", str_c(query), and_args ? "and" : "or", str_c(fields_not));
    }
    str_printfa(str, "}}, \"size\":%lu}", num_rows);

    ret = elastic_connection_select(backend->elastic_conn, pool,
                                          str, box_guid, &es_results);

    /* build our fts_result return */
    result->box = box;
    result->scores_sorted = FALSE;

    /* FTS_LOOKUP_FLAG_NO_AUTO_FUZZY says that exact matches for non-fuzzy searches
     * should go to maybe_uids instead of definite_uids. */
    ARRAY_TYPE(seq_range) *uids_arr = (flags & FTS_LOOKUP_FLAG_NO_AUTO_FUZZY) == 0 ?
            &result->definite_uids : &result->maybe_uids;

    if (ret > 0 && es_results != NULL) {
        array_append_array(uids_arr, &es_results[0]->uids);
        array_append_array(&result->scores, &es_results[0]->scores);
    }

    /* clean-up */
    pool_unref(&pool);
    str_free(&str);
    str_free(&query);
    str_free(&fields);

    return ret;
}

struct fts_backend fts_backend_elastic = {
    .name = "elastic",
    .flags = FTS_BACKEND_FLAG_FUZZY_SEARCH,

    {
        fts_backend_elastic_alloc,
        fts_backend_elastic_init,
        fts_backend_elastic_deinit,
        fts_backend_elastic_get_last_uid,
        fts_backend_elastic_update_init,
        fts_backend_elastic_update_deinit,
        fts_backend_elastic_update_set_mailbox,
        fts_backend_elastic_update_expunge,
        fts_backend_elastic_update_set_build_key,
        fts_backend_elastic_update_unset_build_key,
        fts_backend_elastic_update_build_more,
        fts_backend_elastic_refresh,
        fts_backend_elastic_rescan,
        fts_backend_elastic_optimize,
        fts_backend_default_can_lookup,
        fts_backend_elastic_lookup,
        NULL
    }
};
