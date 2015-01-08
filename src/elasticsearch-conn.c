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

struct elasticsearch_connection_post {
    struct elasticsearch_connection *conn;

    struct http_client_request *http_req;

    unsigned int failed:1;
};

struct elasticsearch_connection {
    char *http_host;
    in_port_t http_port;
    char *http_base_url;
    char *http_failure;

    int request_status;

    struct istream *payload;
    struct io *io;

    unsigned int debug:1;
    unsigned int posting:1;
    unsigned int xml_failed:1;
    unsigned int http_ssl:1;
};

int elasticsearch_connection_init(const char *url, bool debug,
    struct elasticsearch_connection **conn_r, const char **error_r)
{
    struct http_client_settings http_set;
    struct elasticsearch_connection *conn;
    struct http_url *http_url;
    const char *error;

    debug = TRUE; /* TODO: remove! */

    if (http_url_parse(url, NULL, 0, pool_datastack_create(),
               &http_url, &error) < 0) {
        *error_r = t_strdup_printf(
            "fts_elasticsearch: Failed to parse HTTP url: %s", error);

        return -1;
    }

    conn = i_new(struct elasticsearch_connection, 1);
    conn->http_host = i_strdup(http_url->host_name);
    conn->http_port = http_url->port;
    conn->http_base_url = i_strconcat(http_url->path, http_url->enc_query, NULL);
    conn->http_ssl = http_url->have_ssl;
    conn->debug = debug;

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
    i_free(conn->http_host);
    i_free(conn->http_base_url);
    i_free(conn);
}

static void
elasticsearch_connection_update_response(const struct http_response *response,
                struct elasticsearch_connection *conn)
{
    if (response->status / 100 != 2) {
        i_error("fts_elasticsearch: Indexing failed: %s", response->reason);
        conn->request_status = -1;
    }
}

static struct http_client_request *
elasticsearch_connection_post_request(struct elasticsearch_connection *conn,
    const char * box_guid, const uint32_t message_uid)
{
    struct http_client_request *http_req;
    const char *url;

    url = t_strconcat(conn->http_base_url, "/_bulk/", NULL);

    http_req = http_client_request(elasticsearch_http_client, "POST",
                       conn->http_host, url,
                       elasticsearch_connection_update_response, conn);
    http_client_request_set_port(http_req, conn->http_port);
    http_client_request_set_ssl(http_req, conn->http_ssl);
    http_client_request_add_header(http_req, "Content-Type", "text/json");
    return http_req;
}

int elasticsearch_connection_post(struct elasticsearch_connection *conn,
    const char *cmd, const char * box_guid, const uint32_t message_uid)
{
    struct http_client_request *http_req;
    struct istream *post_payload;

    http_req = elasticsearch_connection_post_request(conn, box_guid, message_uid);

    /*debug("POSTING: %s\n", cmd);*/

    post_payload = i_stream_create_from_data(cmd, strlen(cmd));

    http_client_request_set_payload(http_req, post_payload, TRUE);

    i_stream_unref(&post_payload);

    http_client_request_submit(http_req);

    conn->request_status = 0;

    http_client_wait(elasticsearch_http_client);

    return conn->request_status;
}

static void
elasticsearch_connection_select_response(const struct http_response *response,
                struct elasticsearch_connection *conn)
{

}

int elasticsearch_connection_select(struct elasticsearch_connection *conn,
    const char *query, const char *box_guid, struct elasticsearch_result ***box_results_r)
{
    struct http_client_request *http_req;
    struct istream *post_payload;
    const char *url;

    url = t_strconcat(conn->http_base_url, box_guid, NULL);
    url = t_strconcat(url, "/mail/_search", NULL);

    i_debug("search query: %s", query);
    i_debug("hitting url: %s", url);

    /*

    

    http_req = http_client_request(elasticsearch_http_client, "POST",
                       conn->http_host, url,
                       elasticsearch_connection_update_response, conn);
    http_client_request_set_port(http_req, conn->http_port);
    http_client_request_set_ssl(http_req, conn->http_ssl);
    http_client_request_add_header(http_req, "Content-Type", "text/json");*/

    /*debug("GETTING: %s\n", query);*/

    /*post_payload = i_stream_create_from_data(query, strlen(query));

    http_client_request_set_payload(http_req, post_payload, TRUE);

    i_stream_unref(&post_payload);

    http_client_request_submit(http_req);

    conn->request_status = 0;

    http_client_wait(elasticsearch_http_client);

    return conn->request_status;*/
}