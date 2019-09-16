/* Copyright (c) 2006-2014 Dovecot authors, see the included COPYING file */
/* Copyright (c) 2014 Joshua Atkins <josh@ascendantcom.com> */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "ioloop.h"
#include "istream.h"
#include "http-url.h"
#include "http-client.h"
#include "fts-elastic-plugin.h"
#include "elastic-connection.h"

#include <json-c/json.h>
#include <stdio.h>


struct elastic_lookup_context {
    pool_t result_pool;

	/* box_id -> elastic_result */
    HASH_TABLE(char *, struct elastic_result *) mailboxes;

    /* temporary results */
    ARRAY(struct elastic_result *) results;

    /* current mailbox */
    const char *box_guid;

    /* current message */
    int32_t uid;
    float score;

    bool results_found;
};


struct elastic_connection {
    /* ElasticSearch HTTP API information */
    char *http_host;
    in_port_t http_port;
    char *http_base_url;
    char *url_params; /* contains elastic routing key appended to urls */
    char *http_failure;
    int request_status;

    /* for streaming processing of results */
    struct istream *payload;
    struct io *io;
	struct json_tokener *tok;

    enum elastic_post_type post_type;

    /* context for the current lookup */
    struct elastic_lookup_context *ctx;

    /* if we should send ?refresh=true on update _bulk requests */
    unsigned int refresh_on_update:1;
    unsigned int debug:1;
    unsigned int http_ssl:1;
};


int elastic_connection_init(const struct fts_elastic_settings *set,
                            const char * routing_key,
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
#if defined(DOVECOT_PREREQ) && DOVECOT_PREREQ(2,3)
    conn->http_host = i_strdup(http_url->host.name);
#else
    conn->http_host = i_strdup(http_url->host_name);
#endif
    conn->http_port = http_url->port;
    conn->http_base_url = i_strconcat(http_url->path, http_url->enc_query, NULL);
    conn->http_ssl = http_url->have_ssl;
    conn->url_params = i_strconcat("?routing=", routing_key, NULL);
    conn->debug = set->debug;
    conn->refresh_on_update = set->refresh_on_update;
    conn->tok = json_tokener_new();

    /* guard against init being called multiple times */
    if (elastic_http_client == NULL) {
        memset(&http_set, 0, sizeof(http_set));
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
        i_free(conn->http_base_url);
        i_free(conn->url_params);
        json_tokener_free(conn->tok);
        i_free(conn);
    }
}


static void
elastic_connection_update_response(const struct http_response *response,
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


static void elastic_connection_payload_input(struct elastic_connection *conn)
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
            jobj_parse(conn, jobj);
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


static void
elastic_connection_select_response(const struct http_response *response,
                                   struct elastic_connection *conn)
{
    /* 404's on non-updates mean the index doesn't exist and should be indexed. 
     * we don't want to flood the error log with useless messages since dovecot
     * will redo the query automatically after indexing it. */
    if (conn->post_type != ELASTIC_POST_TYPE_UPDATE && response->status == 404) {
        conn->request_status = -1;
        return;
    }

    /* 404's usually mean the index is missing. it could mean you also hit a
     * non-ES service but this seems better than a second indices exists lookup */
    if (response->status / 100 != 2) {
        i_error("fts_elastic: lookup failed: %s", response->reason);
        conn->request_status = -1;
        return;
    }

    if (response->payload == NULL) {
        i_error("fts_elastic: lookup failed: empty response payload");
        conn->request_status = -1;
        return;
    }

    i_stream_ref(response->payload);
    conn->payload = response->payload;
    conn->io = io_add_istream(response->payload,
                    elastic_connection_payload_input, conn);
    elastic_connection_payload_input(conn);
}


static void
elastic_connection_http_response(const struct http_response *response,
                                 struct elastic_connection *conn)
{
    if (response != NULL && conn != NULL) {
        switch (conn->post_type) {
        case ELASTIC_POST_TYPE_LAST_UID: /* fall through */
        case ELASTIC_POST_TYPE_SELECT:
            elastic_connection_select_response(response, conn);
            break;
        case ELASTIC_POST_TYPE_UPDATE:
            elastic_connection_update_response(response, conn);
            break;
        case ELASTIC_POST_TYPE_REFRESH:
            /* not implemented */
            break;
        }
    }
}


int elastic_connection_post(struct elastic_connection *conn,
                            const char *url, string_t *data)
{
    struct http_client_request *http_req = NULL;
    struct istream *post_payload = NULL;

    if (conn == NULL || url == NULL || data == NULL) {
        i_error("fts_elastic: connection_post: critical error during POST");
        return -1;
    }

    http_req = http_client_request(elastic_http_client, "POST", conn->http_host,
                                   url, elastic_connection_http_response, conn);
    http_client_request_set_port(http_req, conn->http_port);
    http_client_request_set_ssl(http_req, conn->http_ssl);
    http_client_request_add_header(http_req, "Content-Type", "application/json");

    post_payload = i_stream_create_from_buffer(data);
    http_client_request_set_payload(http_req, post_payload, TRUE);
    i_stream_unref(&post_payload);
    http_client_request_submit(http_req);

    conn->request_status = 0;
    http_client_wait(elastic_http_client);

    return conn->request_status;
}


static struct elastic_result *
elastic_result_get(struct elastic_connection *conn, const char *box_id)
{
    struct elastic_result *result = NULL;
    char *box_id_dup = NULL;

    /* check if the mailbox is cached first */ 
    result = hash_table_lookup(conn->ctx->mailboxes, box_id);

    if (result != NULL) {
        return result;
    } else {
        /* mailbox is not cached, we have to query it and then cache it */
    }

    box_id_dup = p_strdup(conn->ctx->result_pool, box_id);
    result = p_new(conn->ctx->result_pool, struct elastic_result, 1);
    result->box_id = box_id_dup;

    p_array_init(&result->uids, conn->ctx->result_pool, 32);
    p_array_init(&result->scores, conn->ctx->result_pool, 32);
    hash_table_insert(conn->ctx->mailboxes, box_id_dup, result);
    array_append(&conn->ctx->results, &result, 1);

    return result;
}


void elastic_connection_last_uid_json(struct elastic_connection *conn,
                                      struct json_object *hits)
{
    int hits_count = 0;
    struct json_object *hit = NULL;

    if (conn == NULL || hits == NULL) {
        i_error("fts_elastic: last_uid_json: critical error while getting last uid");
        return;
    }

    if (json_object_get_type(hits) != json_type_array) {
        i_error("fts_elastic: last_uid_json: hits are not an array");
        return;
    }

    hits_count = json_object_array_length(hits);
    if (hits_count <= 0) {
        i_warning("fts_elastic: last_uid_json: hits are empty array");
        return;
    }

    hit = json_object_array_get_idx(hits, 0);
    /* only interested in the first uid field */
    json_object_object_foreach(hit, key, val) {
        if (strcmp(key, "_id") == 0) {
            conn->ctx->uid = json_object_get_int(val);
            return;
        }
    }
}


void elastic_connection_select_json(struct elastic_connection *conn,
                                    struct json_object *hits)
{
    struct elastic_result *result = NULL;
    struct fts_score_map *tmp_score = NULL;
    struct json_object *hit;
    int hits_count = 0;
    int i = 0;
    const char *_id;
    const char *const *tmp;

    if (conn == NULL || hits == NULL) {
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
        json_object_object_foreach(hit, key, val) {
            if (strcmp(key, "_id") == 0) {
                // conn->ctx->uid = json_object_get_int(val);
                _id = json_object_get_string(val);
                tmp = t_strsplit_spaces(_id, "/");
                if (str_to_int(*tmp, &conn->ctx->uid) < 0 || conn->ctx->uid == 0) {
                    i_warning("fts_elastic: uid <= 0 in _id:\"%s\"", _id);
                    continue;
                }
                tmp++;
                if (*tmp == NULL) {
                    i_warning("fts_elastic: mbox_guid not found in _id:\"%s\"", _id);
                    continue;
                }
                conn->ctx->box_guid = p_strdup(conn->ctx->result_pool, *tmp);
                /* parse user from _id
                tmp++;
                if (*tmp == NULL) {
                    i_warning("fts_elastic: user not found in _id:\"%s\"", _id);
                    continue;
                }
                user = p_strdup(conn->ctx->result_pool, *tmp);
                */
            }
            else if (strcmp(key, "_score") == 0) {
                conn->ctx->score = json_object_get_double(val);  
            }
        }

        /* this is all we need for an e-mail result */
        if (conn->ctx->uid != -1 && conn->ctx->score != -1 && conn->ctx->box_guid != NULL) {
            result = elastic_result_get(conn, conn->ctx->box_guid);
            tmp_score = array_append_space(&result->scores);

            seq_range_array_add(&result->uids, conn->ctx->uid);
            tmp_score->uid = conn->ctx->uid;
            tmp_score->score = conn->ctx->score;

            /* clean-up */
            conn->ctx->uid = -1;
            conn->ctx->score = -1;
            conn->ctx->box_guid = NULL;
            conn->ctx->results_found = TRUE;
        }
    }
}


void jobj_parse(struct elastic_connection *conn, json_object *jobj)
{
    struct json_object *jvalue = NULL;

    i_assert(jobj != NULL);

    /* Check errors */
    if (json_object_object_get_ex(jobj, "errors", &jvalue)) {
        i_warning("fts_elastic: errors in response: %s",
                    json_object_to_json_string(jvalue));
    }

    switch (conn->post_type) {
    case ELASTIC_POST_TYPE_LAST_UID:
    case ELASTIC_POST_TYPE_SELECT:
        if (!json_object_object_get_ex(jobj, "hits", &jvalue)) {
            i_warning("fts_elastic: no .hits in search response");
            break;
        }
        if (!json_object_object_get_ex(jvalue, "hits", &jvalue)) {
            i_warning("fts_elastic: no .hits.hits in search response");
            break;
        }
        if (conn->post_type == ELASTIC_POST_TYPE_LAST_UID) {
            elastic_connection_last_uid_json(conn, jvalue);
        } else {
            elastic_connection_select_json(conn, jvalue);
        }
        break;
    case ELASTIC_POST_TYPE_UPDATE:
        /* not implemented */
        break;
    case ELASTIC_POST_TYPE_REFRESH:
        /* not implemented */
        break;
    }
}


int32_t elastic_connection_get_last_uid(struct elastic_connection *conn, string_t *query)
{
    struct elastic_lookup_context lookup_context;
    const char *url = NULL;

    if (conn == NULL || query == NULL) {
        i_error("fts_elastic: last_uid: critical error while fetching last UID");
        return -1;
    }

    /* set-up the context */
    i_zero(&lookup_context);
    conn->ctx = &lookup_context;
    conn->ctx->uid = -1;
    conn->post_type = ELASTIC_POST_TYPE_LAST_UID;

    /* build the url */
    url = t_strconcat(conn->http_base_url, "_search", conn->url_params, NULL);

    /* perform the actual POST */
    elastic_connection_post(conn, url, query);

    /* set during the json parsing; will be the intiailised -1 or a valid uid */
    return conn->ctx->uid;
}


int elastic_connection_update(struct elastic_connection *conn, string_t *cmd)
{
    const char *url = NULL;

    if (conn != NULL && cmd != NULL) {
        /* set-up the connection */
        conn->post_type = ELASTIC_POST_TYPE_UPDATE;
        url = t_strconcat(conn->http_base_url, "_bulk", conn->url_params, NULL);
        if (conn->refresh_on_update) {
            url = t_strconcat(url, "&refresh=true", NULL);
        }
        elastic_connection_post(conn, url, cmd);
        return conn->request_status;
    } else {
        i_debug("fts_elastic: connection_update: conn is NULL");
        return -1;
    }
}


int elastic_connection_refresh(struct elastic_connection *conn)
{
    const char *url = NULL;
    string_t *query = t_str_new_const("", 0);

    /* validate input */
    if (conn == NULL) {
        i_error("fts_elastic: refresh: critical error");
        return -1;
    }

    /* set-up the context */
    conn->post_type = ELASTIC_POST_TYPE_REFRESH;

    /* build the url; we don't have any choice but to refresh the entire 
     * ES server here because Dovecot's refresh API doesn't give us the
     * mailbox that is being refreshed. */
    url = t_strconcat(conn->http_base_url, "_refresh", conn->url_params, NULL);

    /* perform the actual POST */
    elastic_connection_post(conn, url, query);

    if (conn->request_status < 0) {
        return -1;
    }

    return 0;
}


int elastic_connection_select(struct elastic_connection *conn,
                              pool_t pool, string_t *query,
                              struct elastic_result ***box_results_r)
{
    struct elastic_lookup_context lookup_context;
    const char *url = NULL;

    /* validate our input */
    if (conn == NULL || query == NULL || box_results_r == NULL) {
        i_error("fts_elastic: select: critical error during select");
        return -1;
    }

    /* set-up the context */
	i_zero(&lookup_context);
    conn->ctx = &lookup_context;
    conn->ctx->result_pool = pool;
    conn->ctx->uid = -1;
    conn->ctx->score = -1;
    conn->ctx->results_found = FALSE;
    conn->post_type = ELASTIC_POST_TYPE_SELECT;

    /* initialise our results stores */
    p_array_init(&conn->ctx->results, pool, 32);
    hash_table_create(&lookup_context.mailboxes, default_pool, 0, str_hash, strcmp);

	i_free_and_null(conn->http_failure);
    json_tokener_reset(conn->tok);

    /* build the url */
    url = t_strconcat(conn->http_base_url, "_search", conn->url_params, NULL);

    /* perform the actual POST */
    elastic_connection_post(conn, url, query);

    if (conn->request_status < 0) {
        /* no further processing required */
        return -1;
    }

    /* build our results to push back to the fts api */
    array_append_zero(&conn->ctx->results);
    *box_results_r = array_idx_modifiable(&conn->ctx->results, 0);

    return conn->ctx->results_found;
}
