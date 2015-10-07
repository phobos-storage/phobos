/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright 2014-2015 CEA/DAM. All Rights Reserved.
 */
/**
 * \brief  Phobos Distributed State Service API.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pho_common.h"
#include "pho_dss.h"
#include <errno.h>
#include <stdlib.h>
#include <glib.h>
#include <libpq-fe.h>
#include <jansson.h>

struct dss_result {
    PGresult *pg_res;
    union {
        struct media_info   media[0];
        struct dev_info     dev[0];
        struct object_info  object[0];
        struct layout_info  layout[0];
    } u;
};

#define res_of_item_list(_list) \
    container_of((_list), struct dss_result, u.media)


/**
 * Handle notices from PostgreSQL. Strip the trailing newline and re-emit them
 * through phobos log API.
 */
static void dss_pq_logger(void *arg, const char *message)
{
    size_t mlen = strlen(message);

    if (message[mlen - 1] == '\n')
        mlen -= 1;

    pho_info("%*s", mlen, message);
}

int dss_init(const char *conninfo, void **handle)
{
    PGconn *conn;

    *handle = NULL;

    conn = PQconnectdb(conninfo);

    if (PQstatus(conn) != CONNECTION_OK) {
        /** @todo: figure how to get an errno-like value out of it */
        pho_error(ENOTCONN, "Connection to database failed: %s",
                  PQerrorMessage(conn));
        return -ENOTCONN;
    }

    (void)PQsetNoticeProcessor(conn, dss_pq_logger, NULL);

    *handle = conn;
    return 0;
}

void dss_fini(void *handle)
{
    PQfinish(handle);
}

/**
 * Helper for parsing json, due to missing function to manage uint64 in jansson.
 */
static uint64_t json_dict2uint64(const struct json_t *obj, const char *key,
                                 int *err)
{
    struct json_t *current_obj;
    const char    *val;

    current_obj = json_object_get(obj, key);
    if (current_obj == NULL) {
        pho_debug("Cannot retrieve object '%s'", key);
        (*err)++;
        return 0;
    }

    val = json_string_value(current_obj);
    if (val == NULL) {
        pho_debug("Cannot retrieve value of '%s'", key);
        (*err)++;
        return 0;
    }

    return strtoull(val, NULL, 10);
}

static char *json_dict2char(const struct json_t *obj, const char *key, int *err)
{
    struct json_t   *current_obj;
    char            *val;

    current_obj = json_object_get(obj, key);
    if (!current_obj) {
        pho_debug("Cannot retrieve object '%s'", key);
        (*err)++;
        return NULL;
    }

    val = strdup(json_string_value(current_obj));
    if (!val) {
        pho_debug("Cannot retrieve value of '%s'", key);
        (*err)++;
        return NULL;
    }

    return val;
}


/**
 * Converts one criteria to a psql WHERE clause
 * @param[in]   crit    a single criteria to handle
 * @param[out]  clause  clause is appended here
 */
static int dss_crit_to_pattern(PGconn *conn, const struct dss_crit *crit,
                               GString *clause)
{

    char *escape_string;
    unsigned int escape_len;

    g_string_append_printf(clause, "%s %s ",
                           dss_fields2str(crit->crit_name),
                           dss_cmp2str(crit->crit_cmp));

    switch (dss_fields2type(crit->crit_name)) {
    /** @todo: Use macro from inttypes.h PRIu64, ... ? */
    case DSS_VAL_BIGINT:
        g_string_append_printf(clause, "'%lld'", crit->crit_val.val_bigint);
        break;
    case DSS_VAL_INT:
        g_string_append_printf(clause, "'%ld'", crit->crit_val.val_int);
        break;
    case DSS_VAL_BIGUINT:
        g_string_append_printf(clause, "'%llu'", crit->crit_val.val_biguint);
        break;
    case DSS_VAL_UINT:
        g_string_append_printf(clause, "'%lu'", crit->crit_val.val_uint);
        break;
    case DSS_VAL_ENUM:
        g_string_append_printf(clause, "'%s'",
                               dss_fields_enum2str(crit->crit_name,
                                                   crit->crit_val.val_int));
        break;

    case DSS_VAL_JSON:
        LOG_RETURN(-EINVAL, "Filters apply on keys, not entire JSON objects");

    case DSS_VAL_ARRAY:
    case DSS_VAL_STR:
        /**
         *  According to libpq #1.3.2
         *  "point to" a buffer that is able to hold at least
         *  one more byte than twice the value of length,
         *  otherwise the behavior is undefined.
         */
        escape_len = strlen(crit->crit_val.val_str) * 2 + 1;
        escape_string = malloc(escape_len);
        if (!escape_string)
            return -ENOMEM;

        /** @todo: check error in case of encoding issue ? */
        PQescapeStringConn(conn, escape_string, crit->crit_val.val_str,
                           escape_len, NULL);
        if (dss_fields2type(crit->crit_name) == DSS_VAL_ARRAY)
            g_string_append_printf(clause, "array['%s']", escape_string);
        else
            g_string_append_printf(clause, "'%s'", escape_string);
        free(escape_string);
        break;

    case DSS_VAL_UNKNOWN:
    default:
        LOG_RETURN(-EINVAL, "Attempt to filter on unknown type");
    }

    return 0;
}

/**
 * helper arrays to build SQL query
 */
static const char * const base_query[] = {
    [DSS_DEVICE] = "SELECT family, model, id, adm_status,"
                   " host, path, changer_idx FROM device",
    [DSS_MEDIA]  = "SELECT family, model, id, adm_status,"
                   " address_type, fs_type, fs_status, stats FROM media",
    [DSS_EXTENT] = "SELECT oid, copy_num, state, lyt_type, lyt_info,"
                   "extents FROM extent",
    [DSS_OBJECT] = "SELECT oid, user_md FROM object",
};

static const size_t const res_size[] = {
    [DSS_DEVICE]  = sizeof(struct dev_info),
    [DSS_MEDIA]   = sizeof(struct media_info),
    [DSS_EXTENT]  = sizeof(struct extent),
    [DSS_OBJECT]  = sizeof(struct object_info),
};

static const char * const insert_query[] = {
    [DSS_DEVICE] = "INSERT INTO device (family, model, id, host, adm_status,"
                   " path, changer_idx) VALUES ",
    [DSS_MEDIA]  = "INSERT INTO media (family, model, id, adm_status,"
                   " fs_type, address_type, fs_status, stats) VALUES ",
    [DSS_EXTENT] = "INSERT INTO extent (oid, copy_num, state, lyt_type,"
                   " lyt_info, extents) VALUES ",
    [DSS_OBJECT] = "INSERT INTO object (oid, user_md) VALUES ",

};

static const char * const update_query[] = {
    [DSS_DEVICE] = "UPDATE device SET (family, model, host, adm_status,"
                   " path, changer_idx) ="
                   " ('%s', %s, '%s', '%s', '%s', '%d')"
                   " WHERE id = '%s';",
    [DSS_MEDIA]  = "UPDATE media SET (family, model, adm_status,"
                   " fs_type, address_type, fs_status, stats) ="
                   " ('%s', %s, '%s', '%s', '%s', '%s', '%s')"
                   " WHERE id = '%s';",
    [DSS_EXTENT] = "UPDATE extent SET (copy_num, state, lyt_type,"
                   " lyt_info, extents) ="
                   " ('%d', '%s', '%s', '%s', '%s')"
                   " WHERE oid = '%s';",
    [DSS_OBJECT] = "UPDATE object SET user_md = '%s' "
                   " WHERE oid = '%s';",

};

static const char * const delete_query[] = {
    [DSS_DEVICE] = "DELETE FROM device WHERE id = '%s'; ",
    [DSS_MEDIA]  = "DELETE FROM media WHERE id = '%s'; ",
    [DSS_EXTENT] = "DELETE FROM extent WHERE oid = '%s'; ",
    [DSS_OBJECT] = "DELETE FROM object WHERE oid = '%s'; ",

};

/** @todo We do need Postgresl 9.5 for clean update (upsert) */
static const char * const insert_query_values[] = {
    [DSS_DEVICE] = "('%s', %s, '%s', '%s', '%s', '%s', '%d')%s",
    [DSS_MEDIA]  = "('%s', %s, '%s', '%s', '%s', '%s', '%s', '%s')%s",
    [DSS_EXTENT] = "('%s', '%d', '%s', '%s', '%s', '%s')%s",
    [DSS_OBJECT] = "('%s', '%s')%s",
};

/**
 * Extract media statistics from json
 *
 * \param[in]  stats  media stats to be filled with stats
 * \param[in]  json   String with json media stats
 *
 * \return 0 on success, negative error code on failure.
 */
static int dss_media_stats_decode(struct media_stats *stats, const char *json)
{
    json_t          *root;
    json_error_t     json_error;
    int              parse_error = 0;
    int              rc = 0;
    ENTRY;

    root = json_loads(json, JSON_REJECT_DUPLICATES, &json_error);
    if (!root)
        LOG_RETURN(-EINVAL, "Failed to parse json data: %s", json_error.text);

    if (!json_is_object(root))
        LOG_GOTO(out_decref, rc = -EINVAL, "Invalid stats description");

    stats->nb_obj =
        json_dict2uint64(root, "nb_obj", &parse_error);
    stats->logc_spc_used =
        json_dict2uint64(root, "logc_spc_used", &parse_error);
    stats->phys_spc_used =
       json_dict2uint64(root, "phys_spc_used", &parse_error);
    stats->phys_spc_free =
       json_dict2uint64(root, "phys_spc_free", &parse_error);

    if (parse_error > 0)
        LOG_GOTO(out_decref, rc = -EINVAL,
                 "Json parser: %d missing mandatory fields in media stats",
                 parse_error);

out_decref:
    json_decref(root);
    return rc;
}

static char *dss_media_stats_encode(struct media_stats stats, int *error)
{
    json_t          *root;
    json_error_t     json_error;
    char            *s = NULL;
    char             buffer[32];
    int              err_cnt = 0;
    int              rc;
    ENTRY;

    *error = 0;

    root = json_object();
    if (!root) {
        pho_error(-ENOMEM, "Failed to create json object");
        return NULL;
    }

    /* XXX This hack is needed because jansson lacks support for uint64 */
    snprintf(buffer, sizeof(buffer), "%llu", stats.nb_obj);
    rc = json_object_set_new(root, "nb_obj", json_string(buffer));
    if (rc) {
        pho_error(EINVAL, "Failed to encode 'nb_obj' (%llu)", stats.nb_obj);
        err_cnt++;
    }

    snprintf(buffer, sizeof(buffer), "%llu", stats.logc_spc_used);
    rc = json_object_set_new(root, "logc_spc_used", json_string(buffer));
    if (rc) {
        pho_error(EINVAL, "Failed to encode 'logc_spc_used' (%llu)",
                  stats.logc_spc_used);
        err_cnt++;
    }

    snprintf(buffer, sizeof(buffer), "%llu", stats.phys_spc_used);
    rc = json_object_set_new(root, "phys_spc_used", json_string(buffer));
    if (rc) {
        pho_error(EINVAL, "Failed to encode 'phys_spc_used' (%llu)",
                  stats.phys_spc_used);
        err_cnt++;
    }

    snprintf(buffer, sizeof(buffer), "%llu", stats.phys_spc_free);
    rc = json_object_set_new(root, "phys_spc_free", json_string(buffer));
    if (rc) {
        pho_error(EINVAL, "Failed to encode 'phys_spc_free' (%llu)",
                  stats.phys_spc_free);
        err_cnt++;
    }

    *error = err_cnt;

    s = json_dumps(root, 0);
    json_decref(root);
    if (!s) {
        pho_error(EINVAL, "Failed to dump JSON to ASCIIZ");
        return NULL;
    }

    pho_debug("Created JSON representation for stats: '%s'", s);
    return s;
}

static int dss_layout_extents_decode(struct extent **extents,
                                     unsigned int *count, const char *json)
{
    json_t          *root;
    json_t          *child;
    json_error_t     json_error;
    int              parse_error = 0;
    struct extent   *result = NULL;
    size_t           extents_res_size;
    int              rc;
    int              i;
    ENTRY;

    pho_debug("Decoding JSON representation for extents: '%s'", json);

    root = json_loads(json, JSON_REJECT_DUPLICATES, &json_error);
    if (!root)
        LOG_RETURN(-EINVAL, "Failed to parse json data: %s", json_error.text);

    if (!json_is_array(root))
        LOG_GOTO(out_decref, rc = -EINVAL, "Invalid extents description");

    *count = json_array_size(root);
    if (*count == 0) {
        extents = NULL;
        LOG_GOTO(out_decref, rc = -EINVAL,
                 "json parser: extents array is empty");
    }

    extents_res_size = sizeof(struct extent) * (*count);
    result = malloc(extents_res_size);
    if (result == NULL)
        LOG_GOTO(out_decref, rc = -ENOMEM,
                 "Memory allocation of size %zu failed", extents_res_size);

    for (i = 0; i < *count; i++) {
        child = json_array_get(root, i);
        result[i].layout_idx = i;
        result[i].size = json_dict2uint64(child, "sz", &parse_error);
        result[i].address.buff = json_dict2char(child, "addr", &parse_error);
        result[i].address.size = strlen(result[i].address.buff) + 1;
        result[i].media.type = str2dev_family(json_dict2char(child, "fam",
                                                             &parse_error));

        /*XXX fs_type & address_type retrieved from media info */
        if (result[i].media.type == PHO_DEV_INVAL)
            LOG_GOTO(out_decref, rc = -EINVAL, "Invalid media type");

        rc = media_id_set(&result[i].media, json_dict2char(child, "media",
                                                           &parse_error));
        if (rc)
            LOG_GOTO(out_decref, rc = -EINVAL,
                     "Failed to set media id");
    }

    if (parse_error > 0)
        LOG_GOTO(out_decref, rc = -EINVAL,
                 "json parser: %d missing mandatory fields in extents",
                 parse_error);

    *extents = result;
    rc = 0;

out_decref:
    if (rc)
        free(result);
    json_decref(root);
    return rc;
}

static char *dss_layout_extents_encode(struct extent *extents,
                                       unsigned int count, int *error)
{
    json_t          *root;
    json_t          *child;
    json_error_t     json_error;
    char            *s;
    int              err_cnt = 0;
    struct extent   *result = NULL;
    size_t           extents_res_size;
    char             buffer[32];
    int              rc;
    int              i;
    ENTRY;

    root = json_array();
    if (!root) {
        pho_error(ENOMEM, "Failed to create json root object");
        return NULL;
    }

    for (i = 0; i < count; i++) {
        child = json_object();
        if (!child) {
            pho_error(ENOMEM, "Failed to create json child object");
            err_cnt++;
            continue;
        }

        snprintf(buffer, sizeof(buffer), "%llu", extents[i].size);
        rc = json_object_set_new(child, "sz", json_string(buffer));
        if (rc) {
            pho_error(EINVAL, "Failed to encode 'sz' (%llu)", extents[i].size);
            err_cnt++;
        }

        /* We may have no address yet. */
        if (extents[i].address.buff != NULL) {
            rc = json_object_set_new(child, "addr",
                                     json_string(extents[i].address.buff));
            if (rc) {
                pho_error(EINVAL, "Failed to encode 'addr' (%s)",
                          extents[i].address.buff);
                err_cnt++;
            }
        }

        rc = json_object_set_new(child, "fam",
                         json_string(dev_family2str(extents[i].media.type)));
        if (rc) {
            pho_error(EINVAL, "Failed to encode 'fam' (%d:%s)",
                      extents[i].media.type,
                      dev_family2str(extents[i].media.type));
            err_cnt++;
        }

        rc = json_object_set_new(child, "media",
                         json_string(media_id_get(&extents[i].media)));
        if (rc) {
            pho_error(EINVAL, "Failed to encode 'media' (%s)",
                      media_id_get(&extents[i].media));
            err_cnt++;
        }

        rc = json_array_append(root, child);
        if (rc) {
            pho_error(EINVAL, "Failed to attach child to root object");
            err_cnt++;
        }
    }

    *error = err_cnt;

    s = json_dumps(root, 0);
    json_decref(root);
    if (!s) {
        pho_error(EINVAL, "Failed to dump JSON to ASCIIZ");
        return NULL;
    }

    pho_debug("Created JSON representation for extents: '%s'", s);
    return s;
}

static int get_object_setrequest(void *item_list, int item_cnt,
                                  enum dss_set_action action,
                                  GString *request)
{
    int i;
    ENTRY;

    for (i = 0; i < item_cnt; i++) {
        struct object_info *p_object = (struct object_info *)item_list;

        if (action == DSS_SET_DELETE) {
            g_string_append_printf(request, delete_query[DSS_OBJECT],
                                   p_object[i].oid);
        } else if (action == DSS_SET_INSERT) {
            g_string_append_printf(request, insert_query_values[DSS_OBJECT],
                                   p_object[i].oid, "[]",
                                   i < item_cnt-1 ? "," : ";");
        } else if (action == DSS_SET_UPDATE) {
            g_string_append_printf(request, update_query[DSS_OBJECT],
                                   "[]", p_object[i].oid);
        }
    }
    return 0;
}

static int get_extent_setrequest(void *item_list, int item_cnt,
                                  enum dss_set_action action,
                                  GString *request, int *error)
{
    int i;
    ENTRY;

    for (i = 0; i < item_cnt; i++) {
        struct layout_info  *p_layout = (struct layout_info *)item_list;
        char                *layout;

        if (action == DSS_SET_DELETE) {
            g_string_append_printf(request, delete_query[DSS_EXTENT],
                                   p_layout[i].oid);
        } else if (action == DSS_SET_INSERT) {
            layout = dss_layout_extents_encode(p_layout[i].extents,
                                               p_layout[i].ext_count, error);
            if (!layout)
                LOG_RETURN(-EINVAL, "JSON encoding error");

            g_string_append_printf(request, insert_query_values[DSS_EXTENT],
                                   p_layout[i].oid, p_layout[i].copy_num,
                                   extent_state2str(p_layout[i].state),
                                   layout_type2str(p_layout[i].type), "[]",
                                   layout, i < item_cnt-1 ? "," : ";");
            free(layout);
        } else if (action == DSS_SET_UPDATE) {
            layout = dss_layout_extents_encode(p_layout[i].extents,
                                               p_layout[i].ext_count, error);
            if (!layout)
                LOG_RETURN(-EINVAL, "JSON encoding error");

            g_string_append_printf(request, update_query[DSS_EXTENT],
                                   p_layout[i].copy_num,
                                   extent_state2str(p_layout[i].state),
                                   layout_type2str(p_layout[i].type), "[]",
                                   layout, p_layout[i].oid);
            free(layout);
        }
    }
    return 0;
}

static int get_media_setrequest(void *item_list, int item_cnt,
                                  enum dss_set_action action,
                                  GString *request, int *error)
{
    int i;
    ENTRY;

    for (i = 0; i < item_cnt; i++) {
        struct media_info   *p_media = (struct media_info *)item_list;
        char                *model;

        if (action == DSS_SET_DELETE) {
            g_string_append_printf(request, delete_query[DSS_MEDIA],
                                   media_id_get(&p_media[i].id));
        } else if (action == DSS_SET_INSERT) {
            model = dss_char4sql(p_media[i].model);
            if (!model)
                LOG_RETURN(-ENOMEM, "memory allocation failed");

            g_string_append_printf(request, insert_query_values[DSS_MEDIA],
                                   dev_family2str(p_media[i].id.type),
                                   model, media_id_get(&p_media[i].id),
                                   media_adm_status2str(p_media[i].adm_status),
                                   fs_type2str(p_media[i].fs_type),
                                   address_type2str(p_media[i].addr_type),
                                   fs_status2str(p_media[i].fs_status),
                                   dss_media_stats_encode(p_media[i].stats,
                                                          error),
                                   i < item_cnt-1 ? "," : ";");
            free(model);
        } else if (action == DSS_SET_UPDATE) {
            model = dss_char4sql(p_media[i].model);
            if (!model)
                LOG_RETURN(-ENOMEM, "memory allocation failed");

            g_string_append_printf(request, update_query[DSS_MEDIA],
                                   dev_family2str(p_media[i].id.type), model,
                                   media_adm_status2str(p_media[i].adm_status),
                                   fs_type2str(p_media[i].fs_type),
                                   address_type2str(p_media[i].addr_type),
                                   fs_status2str(p_media[i].fs_status),
                                   dss_media_stats_encode(p_media[i].stats,
                                                          error),
                                   media_id_get(&p_media[i].id));
            free(model);
        }
    }

    return 0;
}

static int get_device_setrequest(void *item_list, int item_cnt,
                                  enum dss_set_action action,
                                  GString *request)
{
    int i;
    ENTRY;

    for (i = 0; i < item_cnt; i++) {
        struct dev_info *p_dev = (struct dev_info *)item_list;
        char            *model;

        if (action == DSS_SET_DELETE) {
            g_string_append_printf(request, delete_query[DSS_DEVICE],
                                   p_dev[i].serial);

        } else if (action == DSS_SET_INSERT) {
            model = dss_char4sql(p_dev[i].model);
            if (!model)
                LOG_RETURN(-ENOMEM, "memory allocation failed");
            g_string_append_printf(request, insert_query_values[DSS_DEVICE],
                                   dev_family2str(p_dev[i].family), model,
                                   p_dev[i].serial, p_dev[i].host,
                                   media_adm_status2str(p_dev[i].adm_status),
                                   p_dev[i].path, p_dev[i].changer_idx,
                                   i < item_cnt-1 ? "," : ";");
            free(model);
        } else if (action == DSS_SET_UPDATE) {
            model = dss_char4sql(p_dev[i].model);
            if (!model)
                LOG_RETURN(-ENOMEM, "memory allocation failed");
            g_string_append_printf(request, update_query[DSS_DEVICE],
                                   dev_family2str(p_dev[i].family),
                                   model, p_dev[i].host,
                                   media_adm_status2str(p_dev[i].adm_status),
                                   p_dev[i].path, p_dev[i].changer_idx,
                                   p_dev[i].serial);
            free(model);
        }
    }

    return 0;
}

static inline bool is_type_supported(enum dss_type type)
{
    switch (type) {
    case DSS_OBJECT:
    case DSS_EXTENT:
    case DSS_DEVICE:
    case DSS_MEDIA:
        return true;

    default:
        return false;
    }
}

int dss_get(void *handle, enum dss_type type, struct dss_crit *crit,
            int crit_cnt, void **item_list, int *item_cnt)
{
    PGconn              *conn = handle;
    PGresult            *res;
    GString             *clause;
    struct dss_result   *dss_res;
    size_t               dss_res_size;
    int                  rc = 0;
    int                  count;
    int                  i;
    ENTRY;

    if (handle == NULL || item_list == NULL || item_cnt == NULL)
        LOG_RETURN(-EINVAL, "Handle: %p, item_list: %p, item_cnt: %p",
                   handle, item_list, item_cnt);

    *item_list = NULL;
    *item_cnt  = 0;

    if (!is_type_supported(type))
        LOG_RETURN(-ENOTSUP, "Unsupported DSS request type %#x", type);

    /* get everything if no criteria */
    clause = g_string_new(base_query[type]);

    if (crit_cnt > 0)
        g_string_append(clause, " WHERE ");

    while (crit_cnt > 0) {
        rc = dss_crit_to_pattern(conn, crit, clause);
        if (rc) {
            pho_error(rc, "failed to append crit %d to clause %s",
                      crit->crit_name, clause->str);
            g_string_free(clause, true);
            return rc;
        }
        crit++;
        crit_cnt--;
        if (crit_cnt > 0)
            g_string_append(clause, " AND ");
    }

    pho_debug("Executing request: '%s'", clause->str);

    res = PQexec(conn, clause->str);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        rc = -ECOMM;
        pho_error(rc, "Query '%s' failed: %s", clause->str,
                  PQerrorMessage(conn));
        PQclear(res);
        g_string_free(clause, true);
        return rc;
    }

    g_string_free(clause, true);

    dss_res_size = sizeof(struct dss_result) + PQntuples(res) * res_size[type];
    dss_res = malloc(dss_res_size);
    if (dss_res == NULL)
        LOG_RETURN(-ENOMEM, "malloc of size %zu failed", dss_res_size);

    switch (type) {
    case DSS_DEVICE:
        dss_res->pg_res = res;
        for (i = 0; i < PQntuples(res); i++) {
            struct dev_info *p_dev = &dss_res->u.dev[i];

            p_dev->family = str2dev_family(PQgetvalue(res, i, 0));
            p_dev->model  = PQgetvalue(res, i, 1);
            p_dev->serial = PQgetvalue(res, i, 2);
            p_dev->adm_status =
                str2adm_status(PQgetvalue(res, i, 3));
            p_dev->host   = PQgetvalue(res, i, 4);
            p_dev->path   = PQgetvalue(res, i, 5);
            /** @todo replace atoi by proper pq function */
            p_dev->changer_idx = atoi(PQgetvalue(res, i, 6));
        }

        *item_list = dss_res->u.dev;
        *item_cnt = PQntuples(res);
        break;

    case DSS_MEDIA:
        dss_res->pg_res = res;
        for (i = 0; i < PQntuples(res); i++) {
            struct media_info *p_media = &dss_res->u.media[i];

            p_media->id.type = str2dev_family(PQgetvalue(res, i, 0));
            p_media->model = PQgetvalue(res, i, 1);
            media_id_set(&p_media->id, PQgetvalue(res, i, 2));
            p_media->adm_status = str2media_adm_status(PQgetvalue(res, i, 3));
            p_media->addr_type = str2address_type(PQgetvalue(res, i, 4));
            p_media->fs_type = str2fs_type(PQgetvalue(res, i, 5));
            p_media->fs_status = str2fs_status(PQgetvalue(res, i, 6));
            rc = dss_media_stats_decode(&p_media->stats, PQgetvalue(res, i, 7));
            if (rc) {
                PQclear(res);
                LOG_GOTO(out, rc, "dss_media stats decode error");
            }
        }

        *item_list = dss_res->u.media;
        *item_cnt = PQntuples(res);
        break;

    case DSS_EXTENT:
        dss_res->pg_res = res;
        for (i = 0; i < PQntuples(res); i++) {
            struct layout_info *p_layout = &dss_res->u.layout[i];

            p_layout->oid =  PQgetvalue(res, i, 0);
            p_layout->copy_num = atoi(PQgetvalue(res, i, 1));
            p_layout->state = str2extent_state(PQgetvalue(res, i, 2));
            p_layout->type = str2layout_type(PQgetvalue(res, i, 3));
            /*@todo info */
            rc = dss_layout_extents_decode(&p_layout->extents,
                                           &p_layout->ext_count,
                                           PQgetvalue(res, i, 5));
            if (rc) {
                PQclear(res);
                LOG_GOTO(out, rc, "dss_extent decode error");
            }
        }

        *item_list = dss_res->u.layout;
        *item_cnt = PQntuples(res);
        break;

    case DSS_OBJECT:
        dss_res->pg_res = res;
        for (i = 0; i < PQntuples(res); i++) {
            struct object_info *p_object = &dss_res->u.object[i];

            p_object->oid = PQgetvalue(res, i, 0);
        }

        *item_list = dss_res->u.object;
        *item_cnt = PQntuples(res);
        break;

    default:
        return -EINVAL;
    }

out:
    return rc;
}

int dss_set(void *handle, enum dss_type type, void *item_list,
            int item_cnt, enum dss_set_action action)
{
    PGconn      *conn = handle;
    GString     *request;
    PGresult    *res = NULL;
    int          error = 0;
    int          rc = 0;
    int          i;
    ENTRY;

    if (handle == NULL || item_list == NULL || item_cnt == 0)
        LOG_RETURN(-EINVAL, "handle: %p, item_list: %p, item_cnt: %d",
                   handle, item_list, item_cnt);

    request = g_string_new("BEGIN;");

    if (action == DSS_SET_INSERT)
        g_string_append(request, insert_query[type]);

    switch (type) {

    case DSS_DEVICE:
        rc = get_device_setrequest(item_list, item_cnt, action, request);
        if (rc)
            LOG_GOTO(out_cleanup, rc, "SQL device request failed");
        break;
    case DSS_MEDIA:
        rc = get_media_setrequest(item_list, item_cnt, action, request, &error);
        if (rc)
            LOG_GOTO(out_cleanup, rc, "SQL media request failed");
        break;
    case DSS_EXTENT:
        rc = get_extent_setrequest(item_list, item_cnt, action, request,
                                   &error);
        if (rc)
            LOG_GOTO(out_cleanup, rc, "SQL extent request failed");
        break;
    case DSS_OBJECT:
        rc = get_object_setrequest(item_list, item_cnt, action, request);
        if (rc)
            LOG_GOTO(out_cleanup, rc, "SQL object request failed");
        break;

    default:
        LOG_RETURN(-ENOTSUP, "unsupported DSS request type %#x", type);

    }

    if (error)
        LOG_GOTO(out_cleanup, rc = -EINVAL,
                 "JSON parsing failed: %d errors found", error);

    pho_debug("Executing request: '%s'", request->str);

    res = PQexec(conn, request->str);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        rc = -ECOMM;
        pho_error(rc, "Query '%s' failed: %s", request->str,
                  PQerrorMessage(conn));
        PQclear(res);

        pho_info("Attempting to rollback after transaction failure");

        res = PQexec(conn, "ROLLBACK; ");
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            pho_error(rc, "Rollback failed");

        goto out_cleanup;
    }

    res = PQexec(conn, "COMMIT; ");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        rc = -ECOMM;
        pho_error(rc, "Commit failed");
    }

out_cleanup:
    PQclear(res);
    g_string_free(request, true);
    return rc;
}

void dss_res_free(void *item_list, int item_cnt)
{
    struct dss_result *dss_res;

    if (item_list) {
        dss_res = res_of_item_list(item_list);

        PQclear(dss_res->pg_res);
        free(dss_res);
    }
}
