#ifndef ELASTICSEARCH_CONN_H
#define ELASTICSEARCH_CONN_H

#include "seq-range-array.h"
#include "fts-api.h"
#include <json-c/json.h>

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

static struct elasticsearch_result *
elasticsearch_result_get(struct elasticsearch_connection *conn, const char *box_id);

int elasticsearch_connection_select(struct elasticsearch_connection *conn, pool_t pool,
    const char *query, const char *box, struct elasticsearch_result ***box_results_r);

int elasticsearch_connection_post(struct elasticsearch_connection *conn,
    const char *cmd, const char * box_guid, const uint32_t message_uid);

void json_parse_array(json_object *jobj, char *key, struct elasticsearch_connection *conn);
void json_parse_result(json_object *source);
void json_parse(json_object * jobj, struct elasticsearch_connection *conn);

#endif