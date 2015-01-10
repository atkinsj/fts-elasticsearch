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
#include "fts-elasticsearch-plugin.h"
#include "elasticsearch-conn.h"

#include <ctype.h>
#include <syslog.h>
#include <unistd.h>
#include <inttypes.h>
#include <json-c/json.h>

struct elasticsearch_fts_backend {
    struct fts_backend backend;
    struct elasticsearch_connection *elasticsearch_conn;
};

struct elasticsearch_fts_backend_update_context {
    struct fts_backend_update_context ctx;

    struct mailbox *prev_box;
    char box_guid[MAILBOX_GUID_HEX_LENGTH+1];
    
    uint32_t prev_uid;

    /* used to build multi-part messages. */
    string_t *temp;
    string_t *current_field;

    /* we store this as a string due to the way ES handles bulk indexing JSON.
     * it is not actually valid JSON and thus can't be built with json-c. */
    string_t *json_request;

    /* builds the current message as a JSON object so we can append it later. */
    json_object *message;
    
    int post;

    uint32_t last_indexed_uid;

    unsigned int last_indexed_uid_set:1;
    unsigned int body_open:1;
    unsigned int documents_added:1;
    unsigned int expunges:1;
    unsigned int truncate_header:1;
};

static struct fts_backend *fts_backend_elasticsearch_alloc(void)
{
    struct elasticsearch_fts_backend *backend;

    backend = i_new(struct elasticsearch_fts_backend, 1);
    backend->backend = fts_backend_elasticsearch;

    return &backend->backend;
}

static int
fts_backend_elasticsearch_init(struct fts_backend *_backend,
                         const char **error_r ATTR_UNUSED)
{
    struct elasticsearch_fts_backend *backend = (struct elasticsearch_fts_backend *)_backend;
    
    struct fts_elasticsearch_user *fuser =
        FTS_ELASTICSEARCH_USER_CONTEXT(_backend->ns->user);

    if (fuser == NULL) {
        *error_r = "Invalid fts_elasticsearch setting";

        return -1;
    }

    return elasticsearch_connection_init(fuser->set.url, fuser->set.debug,
                    &backend->elasticsearch_conn, error_r);
}

static void
fts_backend_elasticsearch_deinit(struct fts_backend *_backend)
{
    struct elasticsearch_fts_backend *backend = (struct elasticsearch_fts_backend *)_backend;

    i_free(backend);
}

static void
fts_backend_elasticsearch_doc_close(struct elasticsearch_fts_backend_update_context *_ctx)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;

    /* convert our completed message to a string and tack it on to our request */
    str_append(ctx->json_request, json_object_to_json_string(ctx->message));
    str_append(ctx->json_request, "\n");

    /* clean-up for the next message */
    str_truncate(ctx->temp, 0);
    str_truncate(ctx->current_field, 0);

    if (ctx->body_open) {
        ctx->body_open = FALSE;
    }
}

/*
 * if the last_uid_r you return does not match the most recent UID Dovecot
 * sees in its index log then you will begin getting update calls to
 * update your index.
 */
static int
fts_backend_elasticsearch_get_last_uid(struct fts_backend *_backend,
                                 struct mailbox *box, uint32_t *last_uid_r)
{
    struct elasticsearch_fts_backend *backend = (struct elasticsearch_fts_backend *)_backend;

    struct fts_index_header hdr;

    /* this should return so long as dovecot's on-disk index is available */
    if (fts_index_get_header(box, &hdr)) {
        *last_uid_r = hdr.last_indexed_uid;

        return 0;
    }

    i_debug("fts-elasticsearch: error in get_last_uid, fts_index_get_header returned false");

    /* TODO: We shouldn't return 0 here; our elasticsearch index will almost
     * certainly be more up to date. We need to crawl ES for the most recent
     * UID we have */
    *last_uid_r = 0;
    (void)fts_index_set_last_uid(box, *last_uid_r);

    return 0;
}

static struct fts_backend_update_context *
fts_backend_elasticsearch_update_init(struct fts_backend *_backend)
{
    /* TODO: update_init only gets called when searching on text?? */
    struct elasticsearch_fts_backend_update_context *ctx;

    ctx = i_new(struct elasticsearch_fts_backend_update_context, 1);
    ctx->ctx.backend = _backend;
    ctx->post = 0;

    return &ctx->ctx;
}

static int
fts_backend_elasticsearch_update_deinit(struct fts_backend_update_context *_ctx)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;
    struct elasticsearch_fts_backend *backend =
        (struct elasticsearch_fts_backend *)_ctx->backend;

    /* this gets called when the last message is finished, so close it up */
    fts_backend_elasticsearch_doc_close(ctx);

    /* do our bulk post */
    elasticsearch_connection_post(backend->elasticsearch_conn, str_c(ctx->json_request), ctx->box_guid, ctx->prev_uid);

    i_free(ctx);
    
    return 0;
}

/**
 * this is called when the mailbox that messages are being indexed to changes.
 */
static void
fts_backend_elasticsearch_update_set_mailbox(struct fts_backend_update_context *_ctx,
                                       struct mailbox *box)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;

    const char *box_guid;

    /* update_set_mailbox has been called but the previous uid is not 0;
     * clean up from our previous mailbox indexing. */
    if (ctx->prev_uid != 0) {
        fts_index_set_last_uid(ctx->prev_box, ctx->prev_uid);
        ctx->prev_uid = 0;
    }

    if (box != NULL) {
        if (fts_mailbox_get_guid(box, &box_guid) < 0) {
            i_debug("fts-elasticsearch: update_set_mailbox: fts_mailbox_get_guid failed");
            _ctx->failed = TRUE;
        }

        /* store the current mailbox we're on in our state struct */
        i_assert(strlen(box_guid) == sizeof(ctx->box_guid) - 1);
        memcpy(ctx->box_guid, box_guid, sizeof(ctx->box_guid) - 1);
    } else {
        /* a box of null appears to indicate that indexing is complete. */

        /* TODO: Figure out why fts-elasticsearch does this. don't we just blat over it later? */
        /*memset(ctx->box_guid, 0, sizeof(ctx->box_guid));*/
    }

    ctx->prev_box = box;    

    return;
}

/**
  * begin building the object we're going to POST.
  */
static void
fts_backend_elasticsearch_doc_open(struct elasticsearch_fts_backend_update_context *_ctx,
              uint32_t uid)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;

    ctx->documents_added = TRUE;

    /* TODO: this json-c code must leak like crazy? i'm not sure how it handles
     * reference counts. */

    json_object *temp = json_object_new_object();

    json_object_object_add(temp, "_index", json_object_new_string(ctx->box_guid));
    json_object_object_add(temp, "_type", json_object_new_string("mail"));
    json_object_object_add(temp, "_id", json_object_new_int(uid));

    json_object *action = json_object_new_object();
    json_object_object_add(action, "index", temp);

    str_append(ctx->json_request, json_object_to_json_string(action));
    str_append(ctx->json_request, "\n");

    json_object *jint = json_object_new_int(uid);
    json_object_object_add(ctx->message, "uid", jint);

    json_object *  jstring = json_object_new_string(ctx->box_guid);
    json_object_object_add(ctx->message, "box", jstring);

    jstring = json_object_new_string(ctx->ctx.backend->ns->owner->username);
    json_object_object_add(ctx->message, "user", jstring);
}

/**
  * called when we've moved on to a new message
  */
static void
fts_backend_elasticsearch_uid_changed(struct elasticsearch_fts_backend_update_context *ctx,
                 uint32_t uid)
{
    /* post is 0, this is the first message TODO: should use documents_added = true? */
    if (ctx->post == 0) {
        i_assert(ctx->prev_uid == 0);

        i_debug("uid changed");

        ctx->current_field = str_new(default_pool, 1024 * 64);
        ctx->temp = str_new(default_pool, 1024 * 64);
        ctx->json_request = str_new(default_pool, 1024 * 64);
        ctx->post = 1;
    } else {
        /* this is the end of an old message. nb: the last message to be indexed
         * will not reach here but will instead be caught in update_deinit. */
        fts_backend_elasticsearch_doc_close(ctx);
    }
    
    ctx->prev_uid = uid;
    ctx->truncate_header = FALSE;
    ctx->message = json_object_new_object();
    fts_backend_elasticsearch_doc_open(ctx, uid);
}

/**
 * This is called once per header field of the message and preceeds the build_more call.
 */
static bool
fts_backend_elasticsearch_update_set_build_key(struct fts_backend_update_context *_ctx,
                                         const struct fts_backend_build_key *key)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;

    /* if the uid doesn't match our expected one, we've moved on to a new message */
    if (key->uid != ctx->prev_uid)
        fts_backend_elasticsearch_uid_changed(ctx, key->uid);

    switch (key->type) {
    case FTS_BACKEND_BUILD_KEY_HDR: /* fall through */
    case FTS_BACKEND_BUILD_KEY_MIME_HDR:
        /* TODO: we don't really want to append, there's probably some way to
         * instantiate a string_t from a const char *. */
        str_append(ctx->current_field, t_str_lcase(key->hdr_name));

        break;
    case FTS_BACKEND_BUILD_KEY_BODY_PART:
        if (!ctx->body_open) {
            ctx->body_open = TRUE;
            str_append(ctx->current_field, "body");
        }

        break;
    case FTS_BACKEND_BUILD_KEY_BODY_PART_BINARY:
        i_unreached();
    }

    return TRUE;
}

/**
 * This is called one or more times per header field.
 */
static int
fts_backend_elasticsearch_update_build_more(struct fts_backend_update_context *_ctx,
                                      const unsigned char *data, size_t size)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;
    
    str_append_n(ctx->temp, data, size);

    /* TODO: we instantiated ctx->temp as 1024 * 64 so we should do some
     * kind of chunking here and make use of update for mid-message posting. */

    return 0;
}

/**
 * This is called once per header field at the completion of value processing.
 */
static void
fts_backend_elasticsearch_update_unset_build_key(struct fts_backend_update_context *_ctx)
{
    struct elasticsearch_fts_backend_update_context *ctx =
        (struct elasticsearch_fts_backend_update_context *)_ctx;

    /* field is complete, add it to our message. */
    json_object *jstring = json_object_new_string(str_c(ctx->temp));
    json_object_object_add(ctx->message, str_c(ctx->current_field), jstring);

    str_truncate(ctx->temp, 0);
    str_truncate(ctx->current_field, 0);

    return;
}

static void
fts_backend_elasticsearch_update_expunge(struct fts_backend_update_context *ctx, uint32_t uid)
{
    i_debug("fts-elasticsearch: update_expunge called");
    /* TODO */
    return;
}

static int fts_backend_elasticsearch_refresh(struct fts_backend *backend ATTR_UNUSED)
{
    i_debug("fts-elasticsearch: refresh called");
    /* TODO */
    return 0;
}

static int fts_backend_elasticsearch_rescan(struct fts_backend *backend)
{
    i_debug("fts-elasticsearch: RESCAN");
    /* TODO */
    return 0;
}

static int fts_backend_elasticsearch_optimize(struct fts_backend *backend ATTR_UNUSED)
{
    i_debug("fts-elasticsearch: optimise called");
    /* TODO */
    return 0;
}

static int
elasticsearch_search(const struct mailbox *box, const char *terms,
               ARRAY_TYPE(seq_range) *uids)
{
    /* TODO: */
    return 0;
}

static bool
elasticsearch_add_definite_query(struct mail_search_arg *arg, json_object *value,
                    json_object *fields)
{
    switch (arg->type) {
    case SEARCH_TEXT:
        /* we don't actually have to do anything here; leaving the fields
         * array blank is sufficient to cause full text search with ES */

        break;
    case SEARCH_BODY:
        /* SEARCH_BODY has a hdr_field_name of null. */
        json_object_array_add(fields, json_object_new_string("body"));

        break;
    case SEARCH_HEADER: /* fall through */
    case SEARCH_HEADER_ADDRESS: /* fall through */
    case SEARCH_HEADER_COMPRESS_LWSP:
        if (!fts_header_want_indexed(arg->hdr_field_name))
            return FALSE;        

        json_object_array_add(fields, json_object_new_string(arg->hdr_field_name));

        break;
    default:
        return FALSE;
    }

    /* TODO: can we wrap a query_string in a not filter? */
    if (arg->match_not)
        i_debug("arg->match_not is true in SEARCH_HEADER_COMPRESS_LWSP");

    /* we always want to add a query value */
    json_object_object_add(value, "query", json_object_new_string(arg->value.str));

    return TRUE;
}

static void
elasticsearch_add_definite_query_args(json_object *term, struct mail_search_arg *arg,
                        bool and_args)
{
    for (; arg != NULL; arg = arg->next) {
        json_object *value = json_object_new_object();
        json_object *fields = json_object_new_array();

        if (elasticsearch_add_definite_query(arg, value, fields)) {
            arg->match_always = TRUE;

            json_object_object_add(value, "fields", fields);
            json_object_object_add(term, "query_string", value);

            if (and_args) {
                i_debug("and args is true");

                /* TODO: figure out what this relates to. */
            }
        }
    }
}

/* 
 * called when an IMAP SEARCH is issued.
 */
static int
fts_backend_elasticsearch_lookup(struct fts_backend *_backend, struct mailbox *box,
            struct mail_search_arg *args,
            enum fts_lookup_flags flags,
            struct fts_result *result)
{

    i_debug("fts-elasticsearch: lookup called");
    i_debug("fts-elasticsearch: lookup: hdr_field_name: %s", args->hdr_field_name);

    struct elasticsearch_fts_backend *backend =
        (struct elasticsearch_fts_backend *)_backend;

    struct mailbox_status status;
    bool and_args = (flags & FTS_LOOKUP_FLAG_AND_ARGS) != 0;
    const char *box_guid;
    int ret;
    pool_t pool = pool_alloconly_create("fts elasticsearch search", 1024);

    if (fts_mailbox_get_guid(box, &box_guid) < 0)
        return -1;

    mailbox_get_open_status(box, STATUS_UIDNEXT, &status);

    /* TODO: pagination, status.uidnext shows where we're up to. */

    /* start building our query object */
    json_object *term = json_object_new_object();
    
    elasticsearch_add_definite_query_args(term, args, and_args);

    json_object *query = json_object_new_object();
    json_object_object_add(query, "query", term);

    /* TODO: we also need to support maybe_uid's */

    ret = elasticsearch_connection_select(backend->elasticsearch_conn, pool,
        json_object_to_json_string(query), box_guid, result);



    /*

    ARRAY_TYPE(seq_range) uids;
    ARRAY_TYPE(fts_score_map) scores;

    struct fts_score_map score;
    score.uid = 42;
    score.score = 100;
    i_array_init(&uids, 1);
    i_array_init(&scores, 1);


    array_append_array(&result->definite_uids, &uids);
    array_append_array(&result->scores, &scores);*/

    /*args->match_always = TRUE;*/

    return 1;
}

static int
elasticsearch_search_multi(struct fts_backend *_backend, string_t *str,
          struct mailbox *const boxes[],
          struct fts_multi_result *result)
{
    i_debug("fts-elasticsearch: search_multi called");


    return 0;
}

static int
fts_backend_elasticsearch_lookup_multi(struct fts_backend *backend,
                  struct mailbox *const boxes[],
                  struct mail_search_arg *args, bool and_args,
                  struct fts_multi_result *result)
{
    i_debug("fts-elasticsearch: lookup multi called");
    /* TODO */
    return 0;
}

struct fts_backend fts_backend_elasticsearch = {
    .name = "elasticsearch",
    .flags = FTS_BACKEND_FLAG_FUZZY_SEARCH,

    {
        fts_backend_elasticsearch_alloc,
        fts_backend_elasticsearch_init,
        fts_backend_elasticsearch_deinit,
        fts_backend_elasticsearch_get_last_uid,
        fts_backend_elasticsearch_update_init,
        fts_backend_elasticsearch_update_deinit,
        fts_backend_elasticsearch_update_set_mailbox,
        fts_backend_elasticsearch_update_expunge,
        fts_backend_elasticsearch_update_set_build_key,
        fts_backend_elasticsearch_update_unset_build_key,
        fts_backend_elasticsearch_update_build_more,
        fts_backend_elasticsearch_refresh,
        fts_backend_elasticsearch_rescan,
        fts_backend_elasticsearch_optimize,
        fts_backend_default_can_lookup,
        fts_backend_elasticsearch_lookup,
        fts_backend_elasticsearch_lookup_multi,
        NULL
    }
};
