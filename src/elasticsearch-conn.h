#ifndef ELASTICSEARCH_CONN_H
#define ELASTICSEARCH_CONN_H

#include "seq-range-array.h"
#include "http-client.h"
#include "fts-api.h"
#include <json-c/json.h>

struct fts_elasticsearch_settings;
struct elasticsearch_connection;

enum elasticsearch_post_type {
    ELASTICSEARCH_POST_TYPE_UPDATE = 0,
    ELASTICSEARCH_POST_TYPE_SELECT,
    ELASTICSEARCH_POST_TYPE_LAST_UID,
    ELASTICSEARCH_POST_TYPE_REFRESH,
};

struct elasticsearch_result {
    const char *box_id;

    ARRAY_TYPE(seq_range) uids;
    ARRAY_TYPE(fts_score_map) scores;
};

int elasticsearch_connection_init(const struct fts_elasticsearch_settings *set,
                                      struct elasticsearch_connection **conn_r,
                                      const char **error_r);

void elasticsearch_connection_deinit(struct elasticsearch_connection *conn);


int elasticsearch_connection_update(struct elasticsearch_connection *conn,
                                        string_t *cmd);

int elasticsearch_connection_post(struct elasticsearch_connection *conn,
                                  const char *url, string_t *cmd);

void json_parse_array(json_object *jobj, char *key,
                      struct elasticsearch_connection *conn);

void elasticsearch_connection_last_uid_json(struct elasticsearch_connection *conn,
                                            char *key, struct json_object *val);

void elasticsearch_connection_select_json(struct elasticsearch_connection *conn,
                                          char *key, struct json_object *val);


void jobj_parse(struct elasticsearch_connection *conn, json_object *jobj);


int32_t elasticsearch_connection_last_uid(struct elasticsearch_connection *conn,
                                      string_t *query, const char *box_guid);

struct http_client_request*
elasticsearch_connection_http_request(struct elasticsearch_connection *conn,
                                      const char *url);

int32_t elasticsearch_connection_refresh(struct elasticsearch_connection *conn);

int32_t elasticsearch_connection_select(struct elasticsearch_connection *conn, pool_t pool,
    string_t *query, const char *box, struct elasticsearch_result ***box_results_r);

#endif
