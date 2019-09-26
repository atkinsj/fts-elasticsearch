/* Copyright (c) 2006-2014 Dovecot authors, see the included COPYING file */
/* Copyright (c) 2014 Joshua Atkins <josh@ascendantcom.com> */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "ioloop.h"
#include "istream.h"
#include "mail-namespace.h"
#include "http-url.h"
#include "http-client.h"
#include "fts-elastic-plugin.h"
#include "elastic-connection.h"

#include <json-c/json.h>
#include <stdio.h>


struct elastic_search_context {
    pool_t pool;
    const char *scroll_id;
	struct fts_result *result;
    int found;
};


struct elastic_connection {
    struct mail_namespace *ns;
    char *username;

    /* ElasticSearch HTTP API information */
    char *http_host;
    in_port_t http_port;
    char *http_base_path;
    char *http_failure;
    int request_status;

    /* for streaming processing of results */
    struct istream *payload;
    struct io *io;
	struct json_tokener *tok;

    enum elastic_post_type post_type;

    /* context for the current search */
    struct elastic_search_context *ctx;

    /* if we should send ?refresh=true on update _bulk requests */
    unsigned int refresh_on_update:1;
    unsigned int debug:1;
    unsigned int http_ssl:1;
};


int elastic_connection_init(const struct fts_elastic_settings *set,
                            struct mail_namespace *ns,
                            struct elastic_connection **conn_r,
                            const char **error_r)
{
    struct http_client_settings http_set;
    struct elastic_connection *conn = NULL;
    struct http_url *http_url = NULL;
    const char *error = NULL;

    if (error_r == NULL || set == NULL || conn_r == NULL) {
        i_debug("fts_elastic: error initialising ElasticSearch connection");
        return -1;
    }

    /* validate the url */
    if (http_url_parse(set->url, NULL, 0, pool_datastack_create(),
               &http_url, &error) < 0) {
        *error_r = t_strdup_printf(
            "fts_elastic: Failed to parse HTTP url: %s", error);
        return -1;
    }

    conn = i_new(struct elastic_connection, 1);
    conn->ctx = i_new(struct elastic_search_context, 1);
    conn->ns = ns;
    conn->username = ns->owner ? ns->owner->username : "-";
#if defined(DOVECOT_PREREQ) && DOVECOT_PREREQ(2,3)
    conn->http_host = i_strdup(http_url->host.name);
#else
    conn->http_host = i_strdup(http_url->host_name);
#endif
    conn->http_port = http_url->port;
    conn->http_base_path = i_strdup(http_url->path);
    conn->http_ssl = http_url->have_ssl;
    conn->debug = set->debug;
    conn->refresh_on_update = set->refresh_on_update;
    conn->tok = json_tokener_new();

    /* guard against init being called multiple times */
    if (elastic_http_client == NULL) {
        i_zero(&http_set);
        http_set.max_idle_time_msecs = 5 * 1000;
        http_set.max_parallel_connections = 1;
        http_set.max_pipelined_requests = 1;
        http_set.max_redirects = 1;
        http_set.max_attempts = 3;
        http_set.debug = set->debug;
		http_set.rawlog_dir = set->rawlog_dir;
        elastic_http_client = http_client_init(&http_set);
    }

    *conn_r = conn;

    return 0;
}


void elastic_connection_deinit(struct elastic_connection *conn)
{
    if (conn != NULL) {
        i_free(conn->http_host);
        i_free(conn->http_base_path);
        i_free(conn->ctx);
        json_tokener_free(conn->tok);
        i_free(conn);
    }
}

/* Checks response status code from _bulk request */
static void
elastic_connection_bulk_response(const struct http_response *response,
                                   struct elastic_connection *conn)
{
    if (response != NULL && conn != NULL ) {
        /* 200 OK, 204 continue */
        if (response->status / 100 != 2) {
            i_error("fts_elastic: Indexing failed: %s", response->reason);
            conn->request_status = -1;
        }
    }
}

/* Parses response payload to json from _search request */
static void
elastic_connection_payload_input(struct elastic_connection *conn)
{
    const unsigned char *data = NULL;
    json_object *jobj = NULL;
    enum json_tokener_error jerr;
    size_t size;
    int ret = -1;

    /* continue appending data so long as it is available */
    while ((ret = i_stream_read_more(conn->payload, &data, &size)) > 0) {
        jobj = json_tokener_parse_ex(conn->tok, (const char *)data, size);
        i_stream_skip(conn->payload, size);

        jerr = json_tokener_get_error(conn->tok);
        if (jerr == json_tokener_continue) {
            if (ret < 0)
                i_error("fts_elastic: json response not finished");
        } else if (jerr == json_tokener_success) {
            /* extract values from resulting json object */
            elastic_connection_json(conn, jobj);
        } else {
            i_error("fts_elastic: json tokener error: %s", json_tokener_error_desc(jerr));
            break;
        }
    }

    if (ret == 0) {
        /* we will be called again for more data */
    } else {
        if (conn->payload->stream_errno != 0) {
            i_error("fts_elastic: failed to read payload from HTTP server: %m");
            conn->request_status = -1;
        }

        /* clean-up */
        io_remove(&conn->io);
        i_stream_unref(&conn->payload);
    }
}

/* Parses HTTP response from _search request */
static void
elastic_connection_search_response(const struct http_response *response,
                                   struct elastic_connection *conn)
{
    /* 404's usually mean the index is missing.
     * it could mean you also hit a non-ES service
     * 400 means request json is malformed */
    if (response->status / 100 != 2) {
        i_error("fts_elastic: search failed: %d %s",
                    response->status, response->reason);
        conn->request_status = -1;
    }

    if (response->payload == NULL) {
        i_error("fts_elastic: search failed: empty response payload");
        conn->request_status = -1;
        return;
    }

    i_stream_ref(response->payload);
    conn->payload = response->payload;
    conn->io = io_add_istream(response->payload,
                    elastic_connection_payload_input, conn);
    elastic_connection_payload_input(conn);
}

/* Callback from HTTP request
 * according to post_type decides which callback calls */
static void
elastic_connection_http_response(const struct http_response *response,
                                 struct elastic_connection *conn)
{
    if (response != NULL && conn != NULL) {
        switch (conn->post_type) {
        case ELASTIC_POST_TYPE_SEARCH:
            elastic_connection_search_response(response, conn);
            break;
        case ELASTIC_POST_TYPE_BULK:
            elastic_connection_bulk_response(response, conn);
            break;
        case ELASTIC_POST_TYPE_REFRESH:
        case ELASTIC_POST_TYPE_DELETE:
        case ELASTIC_POST_TYPE_DELETE_BY_QUERY:
            /* not implemented */
            break;
        }
    }
}

/* Performs HTTP POST/DELETE request with callback */
int elastic_connection_post(struct elastic_connection *conn,
                            const char *path, string_t *data)
{
    struct http_client_request *http_req = NULL;
    struct istream *post_payload = NULL;
    const char *method = "POST";

    if (conn == NULL || path == NULL || data == NULL) {
        i_error("fts_elastic: connection_post: critical error during POST");
        return -1;
    }

    if (conn->post_type == ELASTIC_POST_TYPE_DELETE) {
        method = "DELETE";
    }

    http_req = http_client_request(elastic_http_client, method, conn->http_host,
                                   path, elastic_connection_http_response, conn);
    http_client_request_set_port(http_req, conn->http_port);
    http_client_request_set_ssl(http_req, conn->http_ssl);
    /* XXX: should be application/x-ndjson for bulk updates, but why when this works? */
    http_client_request_add_header(http_req, "Content-Type", "application/json");

    post_payload = i_stream_create_from_buffer(data);
    http_client_request_set_payload(http_req, post_payload, TRUE);
    i_stream_unref(&post_payload);
    http_client_request_submit(http_req);

    conn->request_status = 0;
    http_client_wait(elastic_http_client);

    return conn->request_status;
}

/* Iterates over json array hits
 * and fills fts_result->definite_uids
 * and fts->result->scores if present
 */
void elastic_connection_search_hits(struct elastic_search_context *ctx,
                                    struct json_object *hits)
{
    struct fts_score_map *scores;
    struct json_object *hit;
    struct json_object *jval;
    uint32_t uid = 0;
    int hits_count = 0;
    int i = 0;
    const char *_id;
    const char *const *id_part;

    if (ctx == NULL || hits == NULL) {
        i_error("fts_elastic: select_json: critical error while processing result JSON");
        return;
    }

    if (json_object_get_type(hits) != json_type_array) {
        i_error("fts_elastic: select_json: response hits are not array");
        return;
    }

    hits_count = json_object_array_length(hits);
    for (i = 0; i < hits_count; i++) {
        hit = json_object_array_get_idx(hits, i);
        if (!json_object_object_get_ex(hit, "_id", &jval)) {
            i_warning("fts_elastic: key _id not in search response hit:%s",
                        json_object_to_json_string(hit));
            continue;
        }

        _id = json_object_get_string(jval);
        id_part = t_strsplit_spaces(_id, "/");
        if (str_to_uint32(*id_part, &uid) < 0 || uid == 0) {
            i_warning("fts_elastic: uid <= 0 in _id:\"%s\"", _id);
            continue;
        }
        /* we currently search only in one mbox
        id_part++;
        if (*id_part == NULL) {
            i_warning("fts_elastic: mbox_guid not found in _id:\"%s\"", _id);
            guid = "";
            continue;
        }
        if (strcmp(guid, *id_part) != 0) {
            ctx->result = get_fts_result_by_guid(ctx, *id_part);
        } else {
            // We are using already box result from previous hit
        }
        */
        ctx->found += 1;
        if (seq_range_array_add(&ctx->result->definite_uids, uid)) {
            /* duplicate result */
        } else if (json_object_object_get_ex(hit, "_score", &jval)) {
            scores = array_append_space(&ctx->result->scores);
            scores->uid = uid;
            scores->score = json_object_get_double(jval);
        }
        /* parse user from _id
        id_part++;
        if (*id_part == NULL) {
            i_warning("fts_elastic: user not found in _id:\"%s\"", _id);
            continue;
        }
        user = p_strdup(ctx->pool, *id_part);
        */
    }
}


/* extract values from resulting json object */
void elastic_connection_json(struct elastic_connection *conn, json_object *jobj)
{
    struct json_object *jvalue = NULL;

    i_assert(jobj != NULL);

    /* Check for error description */
    if (json_object_object_get_ex(jobj, "error", &jvalue)) {
        i_error("fts_elastic: %s", json_object_get_string(jvalue));
        return;
    }

    /* Check if errors are present in response */
    if (json_object_object_get_ex(jobj, "errors", &jvalue)) {
        i_error("fts_elastic: errors in response");
    }

    /* Check if _scroll_id are present in response */
    if (json_object_object_get_ex(jobj, "_scroll_id", &jvalue)) {
        conn->ctx->scroll_id = p_strdup(conn->ctx->pool, json_object_get_string(jvalue));
    }

    switch (conn->post_type) {
    case ELASTIC_POST_TYPE_SEARCH:
        if (!json_object_object_get_ex(jobj, "hits", &jvalue)) {
            i_error("fts_elastic: no .hits in search response");
            break;
        }
        if (!json_object_object_get_ex(jvalue, "hits", &jvalue)) {
            i_error("fts_elastic: no .hits.hits in search response");
            break;
        }
        elastic_connection_search_hits(conn->ctx, jvalue);
        break;
    case ELASTIC_POST_TYPE_BULK:
    case ELASTIC_POST_TYPE_REFRESH:
    case ELASTIC_POST_TYPE_DELETE:
    case ELASTIC_POST_TYPE_DELETE_BY_QUERY:
        /* not implemented */
        break;
    }
}

/* Performs elastic _bulk request
 * checking only response status
 */
int elastic_connection_bulk(struct elastic_connection *conn, string_t *cmd)
{
    const char *path = NULL;

    if (conn == NULL || cmd == NULL) {
        i_error("fts_elastic: connection_bulk: conn or cmd is NULL");
        return -1;
    }

    conn->post_type = ELASTIC_POST_TYPE_BULK;
    path = t_strconcat(conn->http_base_path, "_bulk"
                        "?routing=", conn->username,
                        conn->refresh_on_update ? "&refresh=true" : "",
                        NULL);
    elastic_connection_post(conn, path, cmd);
    return conn->request_status;
}


int elastic_connection_refresh(struct elastic_connection *conn)
{
    const char *path = NULL;
    string_t *query = t_str_new_const("", 0);

    if (conn == NULL) {
        i_error("fts_elastic: refresh: critical error");
        return -1;
    }

    conn->post_type = ELASTIC_POST_TYPE_REFRESH;
    path = t_strconcat(conn->http_base_path, "_refresh", NULL);
    elastic_connection_post(conn, path, query);

    if (conn->request_status < 0)
        return -1;

    return 0;
}

/* Performs elastic search query
 * parses json response
 * and fills fts_result
 */
int elastic_connection_search(struct elastic_connection *conn,
                              pool_t pool, string_t *query,
                              struct fts_result *result_r)
{
    const char *path = NULL;

    if (conn == NULL || query == NULL || result_r == NULL) {
        i_error("fts_elastic: critical error during search");
        return -1;
    }

    i_zero(conn->ctx);
    conn->ctx->pool = pool;
    conn->ctx->result = result_r;
    conn->ctx->found = 0;
    conn->post_type = ELASTIC_POST_TYPE_SEARCH;

	i_free_and_null(conn->http_failure);
    json_tokener_reset(conn->tok);

    path = t_strconcat(conn->http_base_path, "_search?routing=", conn->username, NULL);
    elastic_connection_post(conn, path, query);

    if (conn->request_status < 0)
        return -1;

    return conn->ctx->found;
}

/* Performs elastic search query with scroll
 * parses json response
 * and fills fts_result
 */
int elastic_connection_search_scroll(struct elastic_connection *conn,
                              pool_t pool, string_t *query,
                              struct fts_result *result_r)
{
    static const char *SCROLL_TIMEOUT = "7s";
    const char *path = NULL;

    if (conn == NULL || query == NULL || result_r == NULL) {
        i_error("fts_elastic: critical error during search scroll");
        return -1;
    }

    i_zero(conn->ctx);
    i_assert(conn->ctx != NULL);
    conn->ctx->pool = pool;
    conn->ctx->result = result_r;
    conn->ctx->found = 0;
    conn->post_type = ELASTIC_POST_TYPE_SEARCH;

	i_free_and_null(conn->http_failure);
    json_tokener_reset(conn->tok);

    path = t_strconcat(conn->http_base_path, "_search?routing=", conn->username,
                        "&scroll=", SCROLL_TIMEOUT, NULL);
    elastic_connection_post(conn, path, query);

    if (conn->ctx->scroll_id == NULL) {
        i_error("fts_elastic: _scroll_id not found in scroll response");
        return 0;
    }

    /* TODO: strip index (last segment) from path to allow elastic behind proxy */
    path = "/_search/scroll";
    while (conn->ctx->found >= 9998) {
        conn->ctx->found = 0;
        str_truncate(query, 0);
        str_printfa(query, "{\"scroll\":\"%s\", \"scroll_id\":\"%s\"}",
                            SCROLL_TIMEOUT,
                            conn->ctx->scroll_id);
        elastic_connection_post(conn, path, query);
    }
    /* DELETE search scroll context */
    conn->post_type = ELASTIC_POST_TYPE_DELETE;
    str_truncate(query, 0);
    str_printfa(query, "{\"scroll_id\":\"%s\"}", conn->ctx->scroll_id);
    elastic_connection_post(conn, path, query);

    if (conn->request_status < 0)
        return -1;

    return conn->ctx->found;
}

/* Performs elastic search delete by query
 */
int elastic_connection_delete_by_query(struct elastic_connection *conn,
                                       pool_t pool, string_t *query)
{
    const char *path = NULL;

    if (conn == NULL || query == NULL) {
        i_error("fts_elastic: critical error during search scroll");
        return -1;
    }

    i_zero(conn->ctx);
    conn->ctx->pool = pool;
    conn->post_type = ELASTIC_POST_TYPE_DELETE_BY_QUERY;

	i_free_and_null(conn->http_failure);

    path = t_strconcat(conn->http_base_path, "_delete_by_query?routing=",
                                                    conn->username, NULL);
    elastic_connection_post(conn, path, query);

    if (conn->request_status < 0)
        return -1;

    return conn->ctx->found;
}
