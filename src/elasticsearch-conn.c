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
#include "elasticsearch-conn.h"

#include <json-c/json.h>
#include <stdio.h>

struct http_client *elasticsearch_http_client = NULL;

struct elasticsearch_lookup_context {
    pool_t result_pool;

    /* results per mailbox */
    HASH_TABLE(char *, struct elasticsearch_result *) mailboxes;

    /* temporary results */
    ARRAY(struct elasticsearch_result *) results;

    /* current mailbox */
    const char *box_guid;

    /* current message */
    int32_t uid;
    float score;

    /* current email as a string */
    string_t *email;
    
    bool results_found;
};

struct elasticsearch_connection {
    /* ElasticSearch HTTP API information */
    char *http_host;
    in_port_t http_port;
    char *http_base_url;
    char *http_failure;
    int32_t request_status;

    /* for streaming processing of results */
    struct istream *payload;
    struct io *io;

    enum elasticsearch_post_type post_type;

    /* context for the current lookup */
    struct elasticsearch_lookup_context *ctx;

    unsigned int debug:1;
    unsigned int http_ssl:1;
};

int32_t elasticsearch_connection_init(const char *url, bool debug,
                                      struct elasticsearch_connection **conn_r,
                                      const char **error_r)
{
    struct http_client_settings http_set;
    struct elasticsearch_connection *conn = NULL;
    struct http_url *http_url = NULL;
    const char *error = NULL;

    if (error_r == NULL || url == NULL || conn_r == NULL) {
        i_debug("fts_elasticsearch: error initialising ElasticSearch connection");
        return -1;
    }

    /* validate the url */
    if (http_url_parse(url, NULL, 0, pool_datastack_create(),
               &http_url, &error) < 0) {
        *error_r = t_strdup_printf(
            "fts_elasticsearch: Failed to parse HTTP url: %s", error);

        return -1;
    }

    conn = i_new(struct elasticsearch_connection, 1);
    conn->http_host = i_strdup(http_url->host.name);
    conn->http_port = http_url->port;
    conn->http_base_url = i_strconcat(http_url->path, http_url->enc_query, NULL);
    conn->http_ssl = http_url->have_ssl;
    conn->debug = debug;

    /* guard against init being called multiple times */
    if (elasticsearch_http_client == NULL) {
        memset(&http_set, 0, sizeof(http_set));
        http_set.max_idle_time_msecs = 5 * 1000;
        http_set.max_parallel_connections = 1;
        http_set.max_pipelined_requests = 1;
        http_set.max_redirects = 1;
        http_set.max_attempts = 3;
        http_set.debug = debug;
        elasticsearch_http_client = http_client_init(&http_set);
    }

    *conn_r = conn;

    return 0;
}

void elasticsearch_connection_deinit(struct elasticsearch_connection *conn)
{
    if (conn != NULL) {
        i_free(conn->http_host);
        i_free(conn->http_base_url);
        i_free(conn);
    }
}

static void
elasticsearch_connection_update_response(const struct http_response *response,
                                         struct elasticsearch_connection *conn)
{
    if (response != NULL && conn != NULL ) {
        /* 200 OK, 204 continue */
        if (response->status / 100 != 2) {
            i_error("fts_elasticsearch: Indexing failed: %s", response->reason);
            conn->request_status = -1;
        }
    }
}

int32_t elasticsearch_connection_update(struct elasticsearch_connection *conn,
                                        const char *cmd)
{
    const char *url = NULL;

    if (conn != NULL && cmd != NULL) {
        /* set-up the connection */
        conn->post_type = ELASTICSEARCH_POST_TYPE_UPDATE;

        url = t_strconcat(conn->http_base_url, "/_bulk/", NULL);

        elasticsearch_connection_post(conn, url, cmd);

        return conn->request_status;
    } else {
        i_debug("fts_elasticsearch: connection_update: conn is NULL");

        return -1;
    }
}

int32_t elasticsearch_connection_post(struct elasticsearch_connection *conn,
                                      const char *url, const char *cmd)
{
    struct http_client_request *http_req = NULL;
    struct istream *post_payload = NULL;

    if (conn == NULL || url == NULL || cmd == NULL) {
        i_error("fts_elasticsearch: connection_post: critical error during POST");

        return -1;
    }

    /* binds a callback object to elasticsearch_connection_http_response */
    http_req = elasticsearch_connection_http_request(conn, url);

    post_payload = i_stream_create_from_data(cmd, strlen(cmd));
    http_client_request_set_payload(http_req, post_payload, TRUE);
    i_stream_unref(&post_payload);
    http_client_request_submit(http_req);

    conn->request_status = 0;
    http_client_wait(elasticsearch_http_client);

    return conn->request_status;
}

void json_parse_array(json_object *jobj, char *key, struct elasticsearch_connection *conn)
{
    enum json_type type;
    json_object *jvalue = NULL, *jarray = NULL;;
    size_t arraylen;
    size_t i;

    /* first array is our entry object */
    jarray = jobj; 

    if (key) {
#if JSON_HAS_GET_EX
        json_object_object_get_ex(jobj, key, &jarray);
#else
        jarray = json_object_object_get(jobj, key);
#endif
    }

    arraylen = json_object_array_length(jarray);

    /* iterate over the array and walk the tree */
    for (i = 0; i < arraylen; i++) {
        jvalue = json_object_array_get_idx(jarray, i);
        type = json_object_get_type(jvalue);

        /* parse further down arrays */
        if (type == json_type_array) {
            json_parse_array(jvalue, NULL, conn);
        }
        else if (type != json_type_object) {
            /* nothing to do it if is not an object */
        } else {
            json_parse(jvalue, conn);
        }
    }
}

static struct elasticsearch_result *
elasticsearch_result_get(struct elasticsearch_connection *conn, const char *box_id)
{
    struct elasticsearch_result *result = NULL;
    char *box_id_dup = NULL;

    /* check if the mailbox is cached first */ 
    result = hash_table_lookup(conn->ctx->mailboxes, box_id);

    if (result != NULL) {
        return result;
    } else {
        /* mailbox is not cached, we have to query it and then cache it */
    }

    box_id_dup = p_strdup(conn->ctx->result_pool, box_id);
    result = p_new(conn->ctx->result_pool, struct elasticsearch_result, 1);
    result->box_id = box_id_dup;

    p_array_init(&result->uids, conn->ctx->result_pool, 32);
    p_array_init(&result->scores, conn->ctx->result_pool, 32);
    hash_table_insert(conn->ctx->mailboxes, box_id_dup, result);
    array_append(&conn->ctx->results, &result, 1);

    return result;
}

void elasticsearch_connection_last_uid_json(struct elasticsearch_connection *conn,
                                            char *key, struct json_object *val)
{
    json_object *jvalue = NULL;

    if (conn != NULL && key != NULL && val != NULL) {
        /* only interested in the uid field */
        if (strcmp(key, "uid") == 0) {
            /* field is returned as an array */
            jvalue = json_object_array_get_idx(val, 0);
            conn->ctx->uid = json_object_get_int(jvalue);
        }
    } else {
        i_error("fts_elasticsearch: last_uid_json: critical error while getting last uid");
    }
}

void elasticsearch_connection_select_json(struct elasticsearch_connection *conn,
                                          char *key, struct json_object *val)
{
    json_object *jvalue;
    struct elasticsearch_result *result = NULL;
    struct fts_score_map *tmp_score = NULL;

    if (conn != NULL) {
        /* ensure a key and val exist before trying to process them */
        if (key != NULL && val != NULL) {
            if (strcmp(key, "uid") == 0) {
                jvalue = json_object_array_get_idx(val, 0);
                conn->ctx->uid = json_object_get_int(jvalue);
            }

            if (strcmp(key, "_score") == 0)
                conn->ctx->score = json_object_get_double(val);  

            if (strcmp(key, "box") == 0) {
                jvalue = json_object_array_get_idx(val, 0);
                conn->ctx->box_guid = json_object_get_string(jvalue);
            }
        }

        /* this is all we need for an e-mail result */
        if (conn->ctx->uid != -1 && conn->ctx->score != -1 && conn->ctx->box_guid != NULL) {
            result = elasticsearch_result_get(conn, conn->ctx->box_guid);
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
    } else { /* conn != NULL && key != NULL && val != NULL */
        i_error("fts_elasticsearch: select_json: critical error while processing result JSON");
    }
}

void json_parse(json_object *jobj, struct elasticsearch_connection *conn)
{
    enum json_type type;
    json_object *temp = NULL;

    json_object_object_foreach(jobj, key, val) {
        /* reinitialise to temp each iteration */
        temp = NULL;

        type = json_object_get_type(val);

        /* the output of the json_parse varies per post type */
        switch (conn->post_type) {
        case ELASTICSEARCH_POST_TYPE_LAST_UID:
            /* we only care about the uid field */
            if (strcmp(key, "uid") == 0) {
                elasticsearch_connection_last_uid_json(conn, key, val);
            }

            break;
        case ELASTICSEARCH_POST_TYPE_SELECT:
            elasticsearch_connection_select_json(conn, key, val);
            break;
        case ELASTICSEARCH_POST_TYPE_UPDATE:
            /* not implemented */
            break;
        case ELASTICSEARCH_POST_TYPE_REFRESH:
            /* not implemented */
            break;
        }

        /* recursively process the json */
        switch (type) {
        case json_type_boolean: /* fall through */
        case json_type_double:  /* fall through */
        case json_type_int:     /* fall through */
        case json_type_string:  /* fall through */
            break; 
        case json_type_object:
#if JSON_HAS_GET_EX
            json_object_object_get_ex(jobj, key, &temp);
#else
            temp = json_object_object_get(jobj, key);
#endif
            
            json_parse(temp, conn);

            break;
        case json_type_array:
            json_parse_array(jobj, key, conn);

            break;
        case json_type_null:
            break;
        }
    }
} 

static int32_t elasticsearch_json_parse(struct elasticsearch_connection *conn,
                                        string_t *data)
{
    json_object *jobj = NULL;

    if (data == NULL) {
        i_error("fts_elasticsearch: json_parse: empty JSON result");

        return -1;
    } else {
        /* attempt to tokenise the JSON */
        jobj = json_tokener_parse(str_c(data));
    }

    if (jobj == NULL) {
        /* TODO: is this because ES only returns partial JSON and chunks it and
         * we just aren't handling the HTTP chunking, or is it JSON chunking. */
        i_error("fts-elasticsearch: unable to parse response JSON");
        i_error("fts_elasticsearch: response JSON: %s", str_c(data));

        return -1;
    }

    /* finish parsing it */
    json_parse(jobj, conn);

    return 0;
}

static void elasticsearch_connection_payload_input(struct elasticsearch_connection *conn)
{
    const unsigned char *data = NULL;
    size_t size;
    int32_t ret = -1;

    /* continue appending data so long as it is available */
    while ((ret = i_stream_read_data(conn->payload, &data, &size, 0)) > 0) {
        str_append(conn->ctx->email, (const char *)data);

        i_stream_skip(conn->payload, size);
    }

    if (ret == 0) {
        /* we will be called again for more data */
    } else {
        if (conn->payload->stream_errno != 0) {
            i_error("fts_elasticsearch: failed to read payload from HTTP server: %m");

            conn->request_status = -1;
        } else {
            /* parse the output */
            elasticsearch_json_parse(conn, conn->ctx->email);
        }

        /* clean-up */
        str_free(&conn->ctx->email);
        io_remove(&conn->io);
        i_stream_unref(&conn->payload);
    }
}

int32_t elasticsearch_connection_last_uid(struct elasticsearch_connection *conn,
                                           const char *query, const char *box_guid)
{
    struct elasticsearch_lookup_context lookup_context;
    const char *url = NULL;

    if (conn == NULL || query == NULL || box_guid == NULL) {
        i_error("fts_elasticsearch: last_uid: critical error while fetching last UID");

        return -1;
    }

    /* set-up the context */
    memset(&lookup_context, 0, sizeof(lookup_context));
    conn->ctx = &lookup_context;
    conn->ctx->uid = -1;
    conn->post_type = ELASTICSEARCH_POST_TYPE_LAST_UID;

    /* build the url */
    url = t_strconcat(conn->http_base_url, box_guid, NULL);
    url = t_strconcat(url, "/mail/_search/", NULL);

    /* perform the actual POST */
    elasticsearch_connection_post(conn, url, query);

    /* set during the json parsing; will be the intiailised -1 or a valid uid */
    return conn->ctx->uid;
}

static void
elasticsearch_connection_select_response(const struct http_response *response,
                                         struct elasticsearch_connection *conn)
{
    /* 404's on non-updates mean the index doesn't exist and should be indexed. 
     * we don't want to flood the error log with useless messages since dovecot
     * will redo the query automatically after indexing it. */
    if (conn->post_type != ELASTICSEARCH_POST_TYPE_UPDATE && response->status == 404) {
        conn->request_status = -1;

        return;
    }

    /* 404's usually mean the index is missing. it could mean you also hit a
     * non-ES service but this seems better than a second indices exists lookup */
    if (response->status / 100 != 2) {
        i_error("fts_elasticsearch: lookup failed: %s", response->reason);
        conn->request_status = -1;
        return;
    }

    if (response->payload == NULL) {
        i_error("fts_elasticsearch: lookup failed: empty response payload");
        conn->request_status = -1;
        return;
    }

    /* TODO: read up in the dovecot source to see how we should clean these up
     * as they are causing I/O leaks. */
    i_stream_ref(response->payload);
    conn->payload = response->payload;
    conn->ctx->email = str_new(default_pool, 1024 * 1024); /* 1Mb */
    conn->io = io_add_istream(response->payload, elasticsearch_connection_payload_input, conn);
    elasticsearch_connection_payload_input(conn);
}

static void
elasticsearch_connection_http_response(const struct http_response *response,
                                       struct elasticsearch_connection *conn)
{
    if (response != NULL && conn != NULL) {
        switch (conn->post_type) {
        case ELASTICSEARCH_POST_TYPE_LAST_UID: /* fall through */
        case ELASTICSEARCH_POST_TYPE_SELECT:
            elasticsearch_connection_select_response(response, conn);
            break;
        case ELASTICSEARCH_POST_TYPE_UPDATE:
            elasticsearch_connection_update_response(response, conn);
            break;
        case ELASTICSEARCH_POST_TYPE_REFRESH:
            /* not implemented */
            break;
        }
    }
}

struct http_client_request*
elasticsearch_connection_http_request(struct elasticsearch_connection *conn,
                                      const char *url)
{
    struct http_client_request *http_req = NULL;

    if (conn != NULL && url != NULL) {
        http_req = http_client_request(elasticsearch_http_client, "POST",
                                       conn->http_host, url,
                                       elasticsearch_connection_http_response,
                                       conn);
        http_client_request_set_port(http_req, conn->http_port);
        http_client_request_set_ssl(http_req, conn->http_ssl);
        http_client_request_add_header(http_req, "Content-Type", "text/json");
    }

    return http_req;
}

int32_t elasticsearch_connection_refresh(struct elasticsearch_connection *conn)
{
    const char *url = NULL;

    /* validate input */
    if (conn == NULL) {
        i_error("fts_elasticsearch: refresh: critical error");

        return -1;
    }

    /* set-up the context */
    conn->post_type = ELASTICSEARCH_POST_TYPE_REFRESH;

    /* build the url; we don't have any choice but to refresh the entire 
     * ES server here because Dovecot's refresh API doesn't give us the
     * mailbox that is being refreshed. */
    url = t_strconcat(conn->http_base_url, "/_refresh/", NULL);

    /* perform the actual POST */
    elasticsearch_connection_post(conn, url, "");

    if (conn->request_status < 0) {
        return -1;
    }

    return 0;
}

int32_t elasticsearch_connection_select(struct elasticsearch_connection *conn,
                                        pool_t pool, const char *query,
                                        const char *box_guid,
                                        struct elasticsearch_result ***box_results_r)
{
    struct elasticsearch_lookup_context lookup_context;
    const char *url = NULL;

    /* validate our input */
    if (conn == NULL || query == NULL || box_guid == NULL || box_results_r == NULL) {
        i_error("fts_elasticsearch: select: critical error during select");

        return -1;
    }

    /* set-up the context */
    memset(&lookup_context, 0, sizeof(lookup_context));
    conn->ctx = &lookup_context;
    conn->ctx->result_pool = pool;
    conn->ctx->uid = -1;
    conn->ctx->score = -1;
    conn->ctx->results_found = FALSE;
    conn->post_type = ELASTICSEARCH_POST_TYPE_SELECT;

    /* initialise our results stores */
    p_array_init(&conn->ctx->results, pool, 32);
    hash_table_create(&lookup_context.mailboxes, default_pool, 0, str_hash, strcmp);

    /* build the url */
    url = t_strconcat(conn->http_base_url, box_guid, NULL);
    url = t_strconcat(url, "/mail/_search/", NULL);

    /* perform the actual POST */
    elasticsearch_connection_post(conn, url, query);

    if (conn->request_status < 0) {
        /* no further processing required */
        return -1;
    }

    /* build our results to push back to the fts api */
    array_append_zero(&conn->ctx->results);
    *box_results_r = array_idx_modifiable(&conn->ctx->results, 0);

    return conn->ctx->results_found;
}
