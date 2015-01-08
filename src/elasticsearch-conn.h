#ifndef ELASTICSEARCH_CONN_H
#define ELASTICSEARCH_CONN_H

#include "seq-range-array.h"
#include "fts-api.h"

struct elasticsearch_connection;

struct elasticsearch_result {
    const char *box_id;

    ARRAY_TYPE(seq_range) uids;
    ARRAY_TYPE(fts_score_map) scores;
};

int elasticsearch_connection_init(const char *url, bool debug,
    struct elasticsearch_connection **conn_r, const char **error_r);

void elasticsearch_connection_deinit(struct elasticsearch_connection *conn);

static void
elasticsearch_connection_select_response(const struct http_response *response,
                struct elasticsearch_connection *conn);

int elasticsearch_connection_select(struct elasticsearch_connection *conn,
    const char *query, const char *box, struct elasticsearch_result ***box_results_r);

static struct http_client_request *
elasticsearch_connection_get_request(struct elasticsearch_connection *conn,
    const char * box_guid, const uint32_t message_uid);

int elasticsearch_connection_post(struct elasticsearch_connection *conn,
    const char *cmd, const char * box_guid, const uint32_t message_uid);

#endif