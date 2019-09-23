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
#include "seq-range-array.h"
#include "unichar.h"
#include "mail-storage-private.h"
#include "mailbox-list-private.h"
#include "mail-search.h"
#include "fts-api.h"
#include "fts-elastic-plugin.h"
#include "elastic-connection.h"

/* values that must be replaced in field names */
static const char *elastic_field_replace_chars = ".#*\"";

struct elastic_fts_backend {
    struct fts_backend backend;
    struct elastic_connection *conn;
};

struct elastic_fts_field {
	char *key;
	string_t *value;
};

struct elastic_fts_backend_update_context {
    struct fts_backend_update_context ctx;

    struct mailbox *prev_box;
    char box_guid[MAILBOX_GUID_HEX_LENGTH + 1];
    char *username;
    
    uint32_t uid;

    /* used to build multi-part messages. */
    string_t *current_key;
    string_t *current_value;

	ARRAY(struct elastic_fts_field) fields;

    /* build a json string for bulk indexing */
    string_t *json_request;

    unsigned int body_open:1;
    unsigned int documents_added:1;
    unsigned int expunges:1;
};

static const char *elastic_field_prepare(const char *field)
{
    int i;

    for (i = 0; elastic_field_replace_chars[i] != '\0'; i++) {
        field = t_str_replace(field, elastic_field_replace_chars[i], '_');
    }

    return t_str_lcase(field);
}

/* values that must be escaped in query and value fields */
static const char elastic_escape_chars[]
    = {'"',   '\\',  '\t', '\b', '\n', '\r', '\f',  0x1C,  0x1D,  0x1E,  0x1F, '\0'};
static const char *elastic_escape_replaces[]
    = {"\\\"","\\\\","\\t","\\b","\\n","\\r","\\f","0x1C","0x1D","0x1E","0x1F", NULL};

/* escape control characters that JSON isn't a fan of */
static const char *elastic_escape(const char *str)
{
    string_t *ret = t_str_new(strlen(str) + 16);
    const char *pos;

    for (; *str != '\0'; str++) {
        if ((pos = strchr(elastic_escape_chars, *str)) != NULL) {
            str_append(ret, elastic_escape_replaces[pos - elastic_escape_chars]);
        } else {
            str_append_c(ret, *str);
        }
    }

    return str_c(ret);
}

static bool elastic_need_escaping(const char *str)
{
	for (; *str != '\0'; str++) {
		if (strchr(elastic_escape_chars, *str) != NULL)
			return TRUE;
	}
	return FALSE;
}

static const char *elastic_maybe_escape(const char *str)
{
    if (elastic_need_escaping(str)) {
        return elastic_escape(str);
    } else {
        return str;
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
fts_backend_elastic_init(struct fts_backend *_backend, const char **error_r)
{
    struct elastic_fts_backend *backend = (struct elastic_fts_backend *)_backend;
    struct fts_elastic_user *fuser = NULL;

    /* ensure our backend is provided */
    if (_backend == NULL) {
        *error_r = "fts_elastic: error during backend initialisation";
        return -1;
    }

    if ((fuser = FTS_ELASTIC_USER_CONTEXT(_backend->ns->user)) == NULL) {
        *error_r = "Invalid fts_elastic setting";
        return -1;
    }

    return elastic_connection_init(&fuser->set, _backend->ns, &backend->conn, error_r);
}

static void
fts_backend_elastic_deinit(struct fts_backend *_backend)
{
    i_free(_backend);
}

static void
fts_backend_elastic_bulk_end(struct elastic_fts_backend_update_context *_ctx)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;
	const struct elastic_fts_field *field;

    /* ensure we have a context */
    if (_ctx == NULL) {
        return;
    }

    array_foreach(&ctx->fields, field) {
        if (str_len(field->value) > 0) {
            str_printfa(ctx->json_request, ",\"%s\":\"%s\"",
                        field->key,
                        elastic_maybe_escape(str_c(field->value))
                        );
            /* keys are reused in following bulk items */
            str_truncate(field->value, 0);
        }
    }

    /* close up this line in the bulk request */
    str_append(ctx->json_request, "}\n");

    /* clean-up for the next message */
    str_truncate(ctx->current_key, 0);
    str_truncate(ctx->current_value, 0);
    ctx->body_open = FALSE;
}

static int
fts_backend_elastic_get_last_uid(struct fts_backend *_backend,
                                 struct mailbox *box,
                                 uint32_t *last_uid_r)
{
    static const char JSON_LAST_UID[] =
        "{"
            "\"sort\":{"
                "\"uid\":\"desc\""
            "},"
            "\"query\":{"
                "\"bool\":{"
                    "\"filter\":["
                        "{\"term\":{\"user\":\"%s\"}},"
                        "{\"term\":{\"box\":\"%s\"}}"
                    "]"
                "}"
            "},"
            "\"_source\":false,"
            "\"size\":1"
        "}";

    struct elastic_fts_backend *backend = (struct elastic_fts_backend *)_backend;
    struct fts_index_header hdr;
    const char *box_guid = NULL;
    pool_t pool;
    string_t *cmd;
    struct fts_result *result;
    int ret;

    /* ensure our backend has been initialised */
    if (_backend == NULL || box == NULL || last_uid_r == NULL) {
        i_error("fts_elastic: critical error in get_last_uid");
        return -1;
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


    pool = pool_alloconly_create("elastic search", 1024);
    cmd = str_new(pool, 256);
    str_printfa(cmd, JSON_LAST_UID, _backend->ns->owner != NULL
                        ? _backend->ns->owner->username : "-", box_guid);

    result = p_new(pool, struct fts_result, 1);
    result->box = box;
    p_array_init(&result->definite_uids, pool, 2);
    p_array_init(&result->maybe_uids, pool, 2);
    p_array_init(&result->scores, pool, 2);

    ret = elastic_connection_search(backend->conn, pool, cmd, result);
    if (seq_range_count(&result->definite_uids) > 0) {
        struct seq_range_iter iter;
        seq_range_array_iter_init(&iter, &result->definite_uids);
        seq_range_array_iter_nth(&iter, 0, last_uid_r);
    } else {
        /* no uid found because they are not indexed yet */
        *last_uid_r = 0;
    }

    pool_unref(&pool);
    str_free(&cmd);
	array_free(&result->definite_uids);
	array_free(&result->maybe_uids);
	array_free(&result->scores);

    if (ret < 0)
        return -1;

    fts_index_set_last_uid(box, *last_uid_r);
    return 0;
}

static struct fts_backend_update_context *
fts_backend_elastic_update_init(struct fts_backend *_backend)
{
    struct elastic_fts_backend_update_context *ctx;

    ctx = i_new(struct elastic_fts_backend_update_context, 1);
    ctx->ctx.backend = _backend;

    /* allocate strings for building messages and multi-part messages
     * with a sensible initial size. */
    ctx->current_key = str_new(default_pool, 64);
    ctx->current_value = str_new(default_pool, 1024 * 64);
    ctx->json_request = str_new(default_pool, 1024 * 64);
    ctx->username = _backend->ns->owner ? _backend->ns->owner->username : "-";
	i_array_init(&ctx->fields, 16);

    return &ctx->ctx;
}

static int
fts_backend_elastic_update_deinit(struct fts_backend_update_context *_ctx)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;
    struct elastic_fts_backend *backend = NULL;
	struct elastic_fts_field *field;

    /* validate our input parameters */
    if (_ctx == NULL || _ctx->backend == NULL) {
        i_error("fts_elastic: critical error in update_deinit");
        return -1;
    }

    backend = (struct elastic_fts_backend *)_ctx->backend;

    /* clean-up: expunges don't need as much clean-up */
    if (!ctx->expunges) {
        /* this gets called when the last message is finished, so close it up */
        fts_backend_elastic_bulk_end(ctx);

        /* cleanup */
        i_zero(&ctx->box_guid);
        str_free(&ctx->current_key);
        str_free(&ctx->current_value);
        array_foreach_modifiable(&ctx->fields, field) {
            str_free(&field->value);
            i_free(field->key);
        }
    	array_free(&ctx->fields);
    }

    /* perform the actual post */
    if (ctx->documents_added)
        elastic_connection_bulk(backend->conn, ctx->json_request);

    /* global clean-up */
    str_free(&ctx->json_request); 
    i_free(ctx);
    
    return 0;
}

static void
fts_backend_elastic_update_set_mailbox(struct fts_backend_update_context *_ctx,
                                       struct mailbox *box)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;
    const char *box_guid = NULL;

    if (_ctx == NULL) {
        i_error("fts_elastic: update_set_mailbox: context was NULL");
        return;
    }

    /* update_set_mailbox has been called but the previous uid is not 0;
     * clean up from our previous mailbox indexing. */
    if (ctx->uid != 0) {
        fts_index_set_last_uid(ctx->prev_box, ctx->uid);
        ctx->uid = 0;
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
        i_zero(&ctx->box_guid);
    }

    ctx->prev_box = box;
}

static void
elastic_add_update_field(struct elastic_fts_backend_update_context *ctx)
{
	struct elastic_fts_field *field;

	/* there are only a few fields. this lookup is fast enough. */
	array_foreach_modifiable(&ctx->fields, field) {
		if (strcasecmp(field->key, str_c(ctx->current_key)) == 0) {
            /* append on new line if adding to existing value */
            if (str_len(field->value) > 0) {
                str_append(field->value, "\n");
            }
			str_append_str(field->value, ctx->current_value);
            return;
        }
	}

	field = i_new(struct elastic_fts_field, 1);
	field->key = i_strdup(str_c(ctx->current_key));
	field->value = str_new(default_pool, 256);
    str_append_str(field->value, ctx->current_value);
	array_push_back(&ctx->fields, field);
    return;
}

static void
fts_backend_elastic_bulk_start(struct elastic_fts_backend_update_context *_ctx,
                               const char *action_name)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;

    /* track that we've added documents */
    ctx->documents_added = TRUE;

    /* add the header that starts the bulk transaction */
    /* _id consists of uid/box_guid/user */
    str_printfa(ctx->json_request, "{\"%s\":{\"_id\":\"%u/%s/%s\"}}\n",
                            action_name, ctx->uid, ctx->box_guid, ctx->username);

    /* expunges don't need anything more than the action line */
    if (!ctx->expunges) {
        /* add first fields; these are static on every message. */
        str_printfa(ctx->json_request,
                    "{\"uid\":%d,"
                    "\"box\":\"%s\","
                    "\"user\":\"%s\""
                    ,
                    ctx->uid,
                    ctx->box_guid,
		            ctx->username
                    );
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
        FTS_ELASTIC_USER_CONTEXT(_ctx->backend->ns->user);

    if (ctx->documents_added) {
        /* this is the end of an old message. nb: the last message to be indexed
         * will not reach here but will instead be caught in update_deinit. */
        fts_backend_elastic_bulk_end(ctx);
    }

    /* chunk up our requests in to reasonable sizes */
    if (str_len(ctx->json_request) > fuser->set.bulk_size) {  
        /* do an early post */
        elastic_connection_bulk(backend->conn, ctx->json_request);

        /* reset our tracking variables */
        str_truncate(ctx->json_request, 0);
    }
    
    ctx->uid = uid;
    
    fts_backend_elastic_bulk_start(ctx, "index");
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
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;

    if (_ctx == NULL || key == NULL) {
        return FALSE;
    }

    /* if the uid doesn't match our expected one, we've moved on to a new message */
    if (key->uid != ctx->uid) {
        fts_backend_elastic_uid_changed(_ctx, key->uid);
    }

    switch (key->type) {
    case FTS_BACKEND_BUILD_KEY_HDR:
    case FTS_BACKEND_BUILD_KEY_MIME_HDR:
        /* Index only wanted headers */
        if (fts_backend_elastic_header_want(key->hdr_name))
            str_append(ctx->current_key, elastic_field_prepare(key->hdr_name));

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

/* build more message body */
static int
fts_backend_elastic_update_build_more(struct fts_backend_update_context *_ctx,
                                      const unsigned char *data, size_t size)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;

    if (_ctx == NULL) {
        i_error("fts_elastic: update_build_more: critical error building message body");
        return -1;
    }

    str_append_max(ctx->current_value, (const char *)data, size);
    return 0;
}

static void
fts_backend_elastic_update_unset_build_key(struct fts_backend_update_context *_ctx)
{
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;

    if (_ctx == NULL) {
        i_error("fts_elastic: unset_build_key _ctx is NULL");
        return;
    }

    /* field is complete, add it to our update fields if not empty. */
    if (str_len(ctx->current_key) > 0) {
        elastic_add_update_field(ctx);
        str_truncate(ctx->current_key, 0);
    }
    str_truncate(ctx->current_value, 0);
}

static void
fts_backend_elastic_update_expunge(struct fts_backend_update_context *_ctx,
                                   uint32_t uid)
{
    /* fix imapc to call update_expunge with each expunged uid */
    struct elastic_fts_backend_update_context *ctx =
        (struct elastic_fts_backend_update_context *)_ctx;

    /* update the context to note that there have been expunges */
    ctx->expunges = TRUE;
    ctx->uid = uid;

    /* add the delete action */
    fts_backend_elastic_bulk_start(ctx, "delete");
}

static int fts_backend_elastic_refresh(struct fts_backend *_backend ATTR_UNUSED)
{
    struct elastic_fts_backend *backend =
        (struct elastic_fts_backend *)_backend;
	struct fts_elastic_user *fuser =
        FTS_ELASTIC_USER_CONTEXT(_backend->ns->user);

    if (fuser->set.refresh_by_fts) {
        elastic_connection_refresh(backend->conn);
    }
    return 0;
}

/* TODO: implement proper rescan */
static int fts_backend_elastic_rescan(struct fts_backend *_backend)
{
    static const char JSON_RESCAN[] =
        "{"
            "\"query\":{"
                "\"bool\":{"
                    "\"filter\":["
                        "{\"term\":{\"user\":\"%s\"}}."
                        "{\"term\":{\"box\":\"%s\"}}"
                    "]"
                "}"
            "},"
            "\"_source\":false,"
            "\"size\":10000"
        "}";

    struct elastic_fts_backend *backend = (struct elastic_fts_backend *)_backend;
	char box_guid[MAILBOX_GUID_HEX_LENGTH+1];
    pool_t pool;
    string_t *cmd;
    uint32_t uid;
    struct fts_result *result;
    int ret;

    /* ensure our backend has been initialised */
    if (_backend == NULL) {
        i_error("fts_elastic: critical error in rescan");
        return -1;
    }

    pool = pool_alloconly_create("elastic search", 1024);
    cmd = str_new(pool, 256);

    // build json query for all user boxes
    str_printfa(cmd, JSON_RESCAN, box_guid,
                _backend->ns->owner ? _backend->ns->owner->username : "-");

    // download all uids for all boxes from elastic
    ret = elastic_connection_search(backend->conn, pool, cmd, result);

    // iterate (box, uid) pairs
    // open box if not opened
    // compare uids find missing/expunged
    // return elastic_connection_rescan(backend->conn, &results);
    // DELETE all other non existing mailboxes from elastic

    pool_unref(&pool);
    str_free(&cmd);

    if (ret < 0)
        return -1;
    return ret;
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
        i_info("fts_elastic: arg->match_not is true");
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
        str_printfa(fields, "\"%s\",", elastic_field_prepare(arg->hdr_field_name));

        break;
    default:
        return FALSE;
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
                str_append(value, elastic_maybe_escape(arg->value.str));
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
                           struct fts_result *result_r)
{
    static const char JSON_MULTI_MATCH[] = 
        "{\"multi_match\":{"
            "\"query\":\"%s\","
            "\"operator\":\"%s\","
            "\"fields\":[%s]"
        "}}";

    struct elastic_fts_backend *backend = (struct elastic_fts_backend *)_backend;
    const char *operator_arg = (flags & FTS_LOOKUP_FLAG_AND_ARGS) ? "and" : "or";
	struct mailbox_status status;
    const char *box_guid = NULL;

    /* temp variables */
    pool_t pool = pool_alloconly_create("fts elastic search", 4096);
    int32_t ret = -1;
    /* json query building */
    string_t *cmd = str_new(pool, 1024);
    string_t *query = str_new(pool, 1024);
    string_t *fields = str_new(pool, 1024);
    string_t *fields_not = str_new(pool, 1024);

    /* validate our input */
    if (_backend == NULL || box == NULL || args == NULL || result_r == NULL) {
        i_error("fts_elastic: critical error during lookup");
        return -1;
    }

    /* get the mailbox guid */
    if (fts_mailbox_get_guid(box, &box_guid) < 0)
        return -1;

    mailbox_get_open_status(box, STATUS_MESSAGES, &status);

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
    str_append(cmd, "{\"query\":{\"bool\":{\"filter\":[");
    str_printfa(cmd, "{\"term\":{\"user\":\"%s\"}},"
                     "{\"term\":{\"box\": \"%s\"}}]",
                        _backend->ns->owner != NULL ? _backend->ns->owner->username : "",
                        box_guid);

    if (str_len(fields) > 0) {
        str_append(cmd, ",\"must\":[");
        str_printfa(cmd, JSON_MULTI_MATCH, str_c(query), operator_arg, str_c(fields));
        str_append(cmd, "]");
    }

    if (str_len(fields_not) > 0) {
        str_append(cmd, ",\"must_not\":[");
        str_printfa(cmd, JSON_MULTI_MATCH, str_c(query), operator_arg, str_c(fields_not));
        str_append(cmd, "]");
    }

    /* default ES is limited to 10,000 results */
    /* TODO: use scroll API */
    str_printfa(cmd, "}}, \"size\":10000, \"_source\":false}");

    /* build our fts_result return */
    result_r->box = box;
    result_r->scores_sorted = FALSE;

    if (status.messages > 10000) {
        ret = elastic_connection_search_scroll(backend->conn, pool, cmd, result_r);
    } else {
        ret = elastic_connection_search(backend->conn, pool, cmd, result_r);
    }

    /* FTS_LOOKUP_FLAG_NO_AUTO_FUZZY says that exact matches for non-fuzzy searches
     * should go to maybe_uids instead of definite_uids. */
    ARRAY_TYPE(seq_range) uids_tmp;
    if ((flags & FTS_LOOKUP_FLAG_NO_AUTO_FUZZY) != 0) {
        uids_tmp = result_r->definite_uids;
        result_r->definite_uids = result_r->maybe_uids;
        result_r->maybe_uids = uids_tmp;
    }

    /* clean-up */
    pool_unref(&pool);
    str_free(&cmd);
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
        /* TODO: fts_backend_elastic_multi_lookup, */
        NULL
    }
};
