#ifndef ELASTIC_CONNECTION_H
#define ELASTIC_CONNECTION_H

#include "seq-range-array.h"
#include "http-client.h"
#include "fts-api.h"
#include <json-c/json.h>

struct fts_elastic_settings;
struct elastic_connection;

enum elastic_post_type {
    ELASTIC_POST_TYPE_UPDATE = 0,
    ELASTIC_POST_TYPE_SELECT,
    ELASTIC_POST_TYPE_LAST_UID,
    ELASTIC_POST_TYPE_REFRESH,
};

struct elastic_result {
    const char *box_id;

    ARRAY_TYPE(seq_range) uids;
    ARRAY_TYPE(fts_score_map) scores;
};

int elastic_connection_init(const struct fts_elastic_settings *set,
                            struct mail_namespace *ns,
                            struct elastic_connection **conn_r,
                            const char **error_r);

void elastic_connection_deinit(struct elastic_connection *conn);

int elastic_connection_update(struct elastic_connection *conn, string_t *cmd);

int elastic_connection_select(struct elastic_connection *conn,
                              pool_t pool, string_t *query,
                              struct elastic_result ***box_results_r);

int elastic_connection_get_last_uid(struct elastic_connection *conn,
                                    string_t *query,
                                    uint32_t *last_uid_r);

int elastic_connection_refresh(struct elastic_connection *conn);

int elastic_connection_post(struct elastic_connection *conn,
                            const char *url, string_t *cmd);

void elastic_connection_last_uid_json(struct elastic_connection *conn,
                                      struct json_object *hits);

void elastic_connection_select_json(struct elastic_connection *conn,
                                    struct json_object *hits);

void jobj_parse(struct elastic_connection *conn, json_object *jobj);

#endif
