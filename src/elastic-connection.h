#ifndef ELASTIC_CONNECTION_H
#define ELASTIC_CONNECTION_H

#include "seq-range-array.h"
#include "http-client.h"
#include "fts-api.h"
#include <json-c/json.h>

struct fts_elastic_settings;
struct elastic_connection;

enum elastic_post_type {
    ELASTIC_POST_TYPE_BULK = 0,
    ELASTIC_POST_TYPE_SEARCH,
    ELASTIC_POST_TYPE_REFRESH,
};

struct elastic_result {
    const char *box_guid;

    ARRAY_TYPE(seq_range) uids;
    ARRAY_TYPE(fts_score_map) scores;
};

struct elastic_search_context;

int elastic_connection_init(const struct fts_elastic_settings *set,
                            struct mail_namespace *ns,
                            struct elastic_connection **conn_r,
                            const char **error_r);

void elastic_connection_deinit(struct elastic_connection *conn);

int elastic_connection_get_last_uid(struct elastic_connection *conn,
                                    string_t *query,
                                    uint32_t *last_uid_r);

int elastic_connection_post(struct elastic_connection *conn,
                            const char *url, string_t *cmd);

void elastic_connection_search_hits(struct elastic_search_context *ctx,
                                    struct json_object *hits);

void jobj_parse(struct elastic_connection *conn, json_object *jobj);

int elastic_connection_bulk(struct elastic_connection *conn, string_t *cmd);

int elastic_connection_refresh(struct elastic_connection *conn);

int elastic_connection_search(struct elastic_connection *conn,
                              pool_t pool, string_t *query,
                              struct fts_result *result_r);

int elastic_connection_rescan(struct elastic_connection *conn,
                              pool_t pool, string_t *query,
                              struct fts_result **results_r);

#endif
