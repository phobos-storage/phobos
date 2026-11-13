/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2026 CEA/DAM.
 *
 *  This file is part of Phobos.
 *
 *  Phobos is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  Phobos is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
 */
/**
 * \brief  Repack of the media of a Phobos instance.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "phobos_admin.h"

#include <errno.h>
#include <glib.h>
#include <string.h>

#include "pho_comm.h"
#include "pho_comm_wrapper.h"
#include "pho_common.h"
#include "pho_dss.h"
#include "pho_dss_wrapper.h"
#include "pho_io.h"
#include "pho_layout.h"
#include "pho_srl_lrs.h"
#include "pho_types.h"
#include "pho_type_utils.h"

#include "utils.h"

static ssize_t _sum_extent_size(struct extent *extents, int count)
{
    ssize_t total_size = 0;
    int i;

    for (i = 0; i < count; ++i)
        total_size += extents[i].size;

    return total_size;
}

static int _get_source_medium(struct admin_handle *adm,
                              const struct pho_id *source,
                              struct pho_ext_loc *loc,
                              struct pho_io_descr *iod,
                              struct io_adapter_module **ioa,
                              enum fs_type *fs_type)
{
    pho_resp_t *resp;
    pho_req_t req;
    int rc;

    pho_srl_request_read_alloc(&req, 1);
    req.id = 1;
    req.ralloc->n_required = 1;
    req.ralloc->operation = PHO_READ_TARGET_ALLOC_OP_READ;
    req.ralloc->med_ids[0]->family = source->family;
    req.ralloc->med_ids[0]->name = xstrdup(source->name);
    req.ralloc->med_ids[0]->library = xstrdup(source->library);

    rc = comm_send_and_recv(&adm->phobosd_comm, &req, &resp);
    if (rc)
        return rc;

    if (pho_response_is_error(resp) && resp->req_id == 1)
        LOG_GOTO(free_resp, rc = resp->error->rc,
                 "Error for read allocation");

    if (!(pho_response_is_read(resp) && resp->req_id == 1))
        LOG_GOTO(free_resp, rc = -EBADMSG,
                 "Bad response for read allocation: ID #%d - '%s'",
                 resp->req_id, pho_srl_response_kind_str(resp));

    *fs_type = (enum fs_type)resp->ralloc->media[0]->fs_type;
    rc = get_io_adapter(*fs_type, ioa);
    if (rc)
        LOG_GOTO(free_resp, rc, "Failed to init read IO adapter");

    loc->root_path = xstrdup(resp->ralloc->media[0]->root_path);
    loc->addr_type = (enum address_type)resp->ralloc->media[0]->addr_type;
    iod->iod_loc = loc;
    /* The medium is only accessed by path: fd 0 would make the xattr
     * reads silently target stdin.
     */
    iod->iod_fd = -1;

free_resp:
    pho_srl_response_free(resp, true);

    return rc;
}

static int _get_target_medium(struct admin_handle *adm,
                              const struct pho_id *source,
                              ssize_t spc_needed,
                              struct string_array *tags,
                              bool empty,
                              const struct pho_id *target_medium,
                              const struct pho_id *avoid_media,
                              int avoid_media_count,
                              struct pho_ext_loc *loc,
                              struct pho_io_descr *iod,
                              struct pho_id *target)
{
    pho_resp_t *resp;
    pho_req_t req;
    int rc;
    int i;

    pho_srl_request_write_alloc(&req, 1, &tags->count, avoid_media_count);
    req.id = 2;
    req.walloc->family = source->family;
    req.walloc->prevent_duplicate = true;
    req.walloc->no_split = true;
    req.walloc->media[0]->size = spc_needed;
    req.walloc->media[0]->empty_medium = target_medium == NULL && empty;
    for (i = 0; i < tags->count; ++i)
        req.walloc->media[0]->tags[i] = xstrdup(tags->strings[i]);

    for (i = 0; i < avoid_media_count; ++i) {
        req.walloc->avoid_med[i]->family = avoid_media[i].family;
        req.walloc->avoid_med[i]->name = xstrdup(avoid_media[i].name);
        req.walloc->avoid_med[i]->library = xstrdup(avoid_media[i].library);
    }

    /* Target the medium requested by the caller, if any: no other medium
     * may be selected, whatever its content.
     */
    if (target_medium != NULL) {
        req.walloc->media[0]->target_med =
            xmalloc(sizeof(*req.walloc->media[0]->target_med));
        pho_resource_id__init(req.walloc->media[0]->target_med);
        req.walloc->media[0]->target_med->family = target_medium->family;
        req.walloc->media[0]->target_med->name = xstrdup(target_medium->name);
        req.walloc->media[0]->target_med->library =
            xstrdup(target_medium->library);
        req.walloc->library = xstrdup(target_medium->library);
    }

    rc = comm_send_and_recv(&adm->phobosd_comm, &req, &resp);
    if (rc)
        return rc;

    if (pho_response_is_error(resp) && resp->req_id == 2)
        LOG_GOTO(free_resp, rc = resp->error->rc,
                 "Error for write allocation");

    if (!(pho_response_is_write(resp) && resp->req_id == 2))
        LOG_GOTO(free_resp, rc = -EBADMSG,
                 "Bad response for write allocation: ID #%d - '%s'",
                 resp->req_id, pho_srl_response_kind_str(resp));

    loc->root_path = xstrdup(resp->walloc->media[0]->root_path);
    loc->addr_type =
        (enum address_type)resp->walloc->media[0]->addr_type;
    iod->iod_flags = PHO_IO_REPLACE | PHO_IO_NO_REUSE;
    iod->iod_loc = loc;
    iod->iod_fd = -1;

    target->family = resp->walloc->media[0]->med_id->family;
    pho_id_name_set(target, resp->walloc->media[0]->med_id->name,
                    resp->walloc->media[0]->med_id->library);

free_resp:
    pho_srl_response_free(resp, true);

    return rc;
}

static int _send_and_recv_release(struct admin_handle *adm,
                                  const struct pho_id *source,
                                  const struct pho_io_descr *iod_source,
                                  int req_id, const struct pho_id *target,
                                  const struct pho_io_descr *iod_target,
                                  ssize_t total_size_written,
                                  ssize_t nb_extents_written)
{
    pho_resp_t *resp;
    pho_req_t req;
    int rc;

    if (target != NULL)
        pho_srl_request_release_alloc(&req, 2, false);
    else
        pho_srl_request_release_alloc(&req, 1, true);
    req.id = req_id;
    req.release->media[0]->med_id->family = source->family;
    req.release->media[0]->med_id->name = xstrdup(source->name);
    req.release->media[0]->med_id->library = xstrdup(source->library);
    req.release->media[0]->rc = iod_source->iod_rc;
    req.release->media[0]->size_written = 0;
    req.release->media[0]->nb_extents_written = 0;
    req.release->media[0]->to_sync = false;

    if (target == NULL) {
        rc = comm_send(&adm->phobosd_comm, &req);

        return rc;
    }

    req.release->media[1]->med_id->family = target->family;
    req.release->media[1]->med_id->name = xstrdup(target->name);
    req.release->media[1]->med_id->library = xstrdup(target->library);
    req.release->media[1]->rc = iod_target->iod_rc;
    req.release->media[1]->size_written = total_size_written;
    req.release->media[1]->nb_extents_written = nb_extents_written;
    req.release->media[1]->to_sync = true;

    rc = comm_send_and_recv(&adm->phobosd_comm, &req, &resp);
    if (rc)
        return rc;

    if (pho_response_is_error(resp) && resp->req_id == req_id)
        LOG_GOTO(free_resp, rc = resp->error->rc,
                 "Error for release request");

    if (!(pho_response_is_release(resp) && resp->req_id == req_id))
        LOG_GOTO(free_resp, rc = -EBADMSG,
                 "Bad response for release request: ID #%d - '%s'",
                 resp->req_id, pho_srl_response_kind_str(resp));

free_resp:
    pho_srl_response_free(resp, true);

    return rc;
}

static int _build_new_extent(const struct pho_id *target,
                             struct extent *old_extent,
                             struct extent *new_extent,
                             struct pho_io_descr *iod_source,
                             struct pho_io_descr *iod_target)
{
    int rc;

    new_extent->uuid = generate_uuid();
    new_extent->state = PHO_EXT_ST_PENDING;
    new_extent->size = old_extent->size;
    new_extent->offset = old_extent->offset;
    new_extent->address.size = old_extent->address.size;
    /* address.buff is left NULL: copy_extent() sets it from the source
     * address once the source is opened, and owns it from then on.
     */
    pho_id_copy(&new_extent->media, target);
    new_extent->with_xxh128 = old_extent->with_xxh128;
    if (new_extent->with_xxh128)
        memcpy(new_extent->xxh128, old_extent->xxh128,
               sizeof(new_extent->xxh128));
    new_extent->with_md5 = old_extent->with_md5;
    if (new_extent->with_md5)
        memcpy(new_extent->md5, old_extent->md5, sizeof(new_extent->md5));
    rc = pho_attrs_copy(&old_extent->info, &new_extent->info);
    if (rc)
        return rc;

    iod_source->iod_loc->extent = old_extent;
    iod_source->iod_size = old_extent->size;
    iod_target->iod_loc->extent = new_extent;

    return 0;
}

static int _clean_database_following_format(struct admin_handle *adm,
                                            const struct pho_id *source)
{
    struct dss_filter filter;
    struct extent *extents;
    int count;
    int rc;

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::EXT::medium_family\": \"%s\"},"
                          "  {\"DSS::EXT::medium_id\": \"%s\"},"
                          "  {\"DSS::EXT::medium_library\": \"%s\"}"
                          "]}", rsc_family2str(source->family), source->name,
                          source->library);
    if (rc)
        LOG_RETURN(rc, "Failed to build orphan extents retrieval filter");

    rc = dss_extent_get(&adm->dss, &filter, &extents, &count, NULL);
    dss_filter_free(&filter);
    if (rc)
        LOG_GOTO(free_ext, rc,
                 "Failed to retrieve orphan extents of source medium");

    if (count == 0)
        goto free_ext;

    rc = dss_extent_delete(&adm->dss, extents, count);
    if (rc)
        pho_error(rc, "Failed to remove orphan extents of source medium");

free_ext:
    dss_res_free(extents, count);

    return rc;
}

static int _retrieve_source_info(struct admin_handle *adm,
                                 const struct pho_id *medium,
                                 struct string_array *tags,
                                 enum fs_type *fs_type,
                                 ssize_t *phys_spc_used)
{
    struct dss_filter filter;
    struct media_info *media;
    int count;
    int rc;

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          "  {\"DSS::MDA::id\": \"%s\"},"
                          "  {\"DSS::MDA::library\": \"%s\"}"
                          "]}", rsc_family2str(medium->family), medium->name,
                          medium->library);
    if (rc)
        LOG_RETURN(rc, "Failed to build medium filter");

    rc = dss_media_get(&adm->dss, &filter, &media, &count, NULL);
    dss_filter_free(&filter);
    if (rc)
        LOG_RETURN(rc,
                   "Failed to retrieve medium "FMT_PHO_ID" info from DSS",
                   PHO_ID(*medium));

    if (count != 1) {
        dss_res_free(media, count);
        LOG_RETURN(-EINVAL, "Did not retrieve one medium, %d instead", count);
    }

    string_array_dup(tags, &media->tags);
    if (fs_type)
        *fs_type = media->fs.type;
    if (phys_spc_used)
        *phys_spc_used = media->stats.phys_spc_used;

    dss_res_free(media, count);

    return 0;
}

static int _resolve_target_medium(struct admin_handle *adm,
                                  const struct pho_id *source,
                                  const struct pho_id *target,
                                  struct pho_id *resolved,
                                  ssize_t *phys_spc_free)
{
    struct dss_filter filter;
    struct media_info *media;
    int count;
    int rc;

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          "  {\"DSS::MDA::id\": \"%s\"}"
                          "]}", rsc_family2str(source->family), target->name);
    if (rc)
        LOG_RETURN(rc, "Failed to build target medium filter");

    rc = dss_media_get(&adm->dss, &filter, &media, &count, NULL);
    dss_filter_free(&filter);
    if (rc)
        LOG_RETURN(rc, "Failed to retrieve target tape '%s' info from DSS",
                   target->name);

    if (count != 1)
        LOG_GOTO(free_media, rc = -EINVAL,
                 "Target tape '%s' does not match a single medium of the DSS",
                 target->name);

    if (pho_id_equal(&media->rsc.id, source))
        LOG_GOTO(free_media, rc = -EINVAL,
                 "Target tape '%s' is the tape being repacked", target->name);

    if (media->rsc.adm_status != PHO_RSC_ADM_ST_UNLOCKED)
        LOG_GOTO(free_media, rc = -EACCES,
                 "Target tape '%s' is administratively locked", target->name);

    if (!media->flags.put)
        LOG_GOTO(free_media, rc = -EPERM,
                 "Put operations are not allowed on target tape '%s'",
                 target->name);

    if (media->fs.status != PHO_FS_STATUS_EMPTY &&
        media->fs.status != PHO_FS_STATUS_USED)
        LOG_GOTO(free_media, rc = -ENOSPC,
                 "Target tape '%s' cannot be written to: filesystem status "
                 "'%s'", target->name, fs_status2str(media->fs.status));

    *phys_spc_free = media->stats.phys_spc_free;
    pho_id_copy(resolved, &media->rsc.id);

free_media:
    dss_res_free(media, count);

    return rc;
}

/* Get the names of the layout-specific attributes of the extent to copy:
 * the layout of an extent is only recorded in its file attributes, which
 * are read to load the matching layout module.
 */
static int _extent_layout_attr_names(struct io_adapter_module *ioa,
                                     const struct pho_ext_loc *loc,
                                     struct pho_attrs *attr_names)
{
    struct pho_ext_loc md_loc = *loc;
    struct pho_io_descr iod = {0};
    const char *layout_name;
    struct pho_attrs md;
    int rc;

    md.attr_set = NULL;
    pho_attr_set(&md, PHO_EA_LAYOUT_NAME, NULL);
    iod.iod_loc = &md_loc;
    iod.iod_attrs = md;
    iod.iod_fd = -1;
    iod.iod_flags = PHO_IO_MD_ONLY;

    rc = ioa_open(ioa, NULL, &iod, false);
    if (rc)
        LOG_RETURN(rc, "Failed to read the layout of the extent to copy");

    layout_name = pho_attr_get(&md, PHO_EA_LAYOUT_NAME);
    if (layout_name == NULL) {
        pho_attrs_free(&md);
        LOG_RETURN(-EINVAL, "The extent to copy has no layout attribute");
    }

    rc = layout_get_extent_attr_names(layout_name, attr_names);

    pho_attrs_free(&md);

    return rc;
}

/* A medium must not receive a copy of the data it already holds: when
 * the LRS selects the target itself, compile the list of media that
 * cannot be used as repack target, and when the target is explicitly
 * given, check that it holds no copy of the data of the source medium.
 */
static int _repack_target_check(struct admin_handle *adm,
                                const struct pho_id *source,
                                const struct pho_id *target,
                                const struct pho_id *target_medium,
                                bool empty, bool ignore_layout_check,
                                struct pho_id **avoid_media, int *avoid_cnt)
{
    bool shared_copy;
    int rc;

    if (ignore_layout_check)
        return 0;

    if (target != NULL) {
        rc = dss_media_check_shared_copy(&adm->dss, source, target_medium,
                                         &shared_copy);
        if (rc)
            LOG_RETURN(rc,
                       "Failed to check that the target tape holds no "
                       "copy of the data of "FMT_PHO_ID, PHO_ID(*source));

        if (shared_copy)
            LOG_RETURN(-EINVAL,
                       "Target tape '%s' already holds a copy of the "
                       "data of the repacked tape", target->name);

        return 0;
    }

    if (!empty) {
        rc = dss_media_get_sharing_objects(&adm->dss, source, avoid_media,
                                           avoid_cnt);
        if (rc)
            LOG_RETURN(rc,
                       "Failed to get the media holding a copy of the "
                       "data of "FMT_PHO_ID, PHO_ID(*source));
    }

    return 0;
}

/* Copy each extent of the source medium to the target medium, inserting
 * the new extents in the DSS and collecting their uuids: ext_cnt_done
 * counts the extents actually copied when the loop is interrupted.
 */
static int _repack_copy_extents(struct admin_handle *adm,
                                struct io_adapter_module *ioa,
                                struct extent *ext_res, int ext_cnt,
                                struct pho_io_descr *iod_source,
                                struct pho_io_descr *iod_target,
                                const struct pho_id *allocated,
                                GArray *new_ext_uuids,
                                int *ext_cnt_done)
{
    int rc = 0;
    int i;

    for (i = 0; i < ext_cnt; ++i, ++(*ext_cnt_done)) {
        struct extent ext_new = {0};
        struct pho_attrs layout_attr_names;

        rc = _build_new_extent(allocated, &ext_res[i], &ext_new, iod_source,
                               iod_target);
        if (rc) {
            pho_error(rc, "Failed to build the extent replacing '%s'",
                      ext_res[i].uuid);
            return rc;
        }

        /* Get the names of the layout-specific attributes of the extent
         * to copy through its layout module: they are retrieved from the
         * source and set on the target along with the common ones.
         */
        layout_attr_names.attr_set = NULL;
        rc = _extent_layout_attr_names(ioa, iod_source->iod_loc,
                                       &layout_attr_names);
        if (rc) {
            pho_error(rc, "Failed to get the layout-specific attributes of "
                      "extent '%s'", ext_res[i].uuid);
            return rc;
        }

        rc = copy_extent(ioa, iod_source, ioa, iod_target, PHO_RSC_TAPE,
                         &layout_attr_names);
        pho_attrs_free(&layout_attr_names);
        if (rc) {
            pho_error(rc, "Failed to copy extent '%s'", ext_res[i].uuid);
            free(ext_new.address.buff);
            return rc;
        }

        rc = dss_extent_insert(&adm->dss, &ext_new, 1, DSS_SET_INSERT);
        if (rc) {
            pho_error(rc, "Failed to add extent '%s' information in DSS",
                      ext_res[i].uuid);
            free(ext_new.address.buff);
            return rc;
        }

        g_array_append_val(new_ext_uuids, ext_new.uuid);

        free(ext_new.address.buff);
        pho_attrs_free(&ext_new.info);
    }

    return rc;
}

/* Database update: swap each old extent of the source medium for the new
 * extent copied on the target medium.
 */
static int _repack_migrate_extents(struct admin_handle *adm,
                                   struct extent *ext_res, int ext_cnt_done,
                                   GArray *new_ext_uuids)
{
    int rc;
    int i;

    for (i = 0; i < ext_cnt_done; ++i) {
        rc = dss_update_extent_migrate(&adm->dss, ext_res[i].uuid,
                                       g_array_index(new_ext_uuids, char *, i));
        if (rc)
            LOG_RETURN(rc, "Failed to update layouts in DSS");
    }

    return 0;
}

int phobos_admin_repack(struct admin_handle *adm, const struct pho_id *source,
                        struct string_array *tags,
                        const struct pho_id *target, bool empty,
                        bool ignore_layout_check)
{
    /* Order the extents to copy by creation time: they are generally
     * sequential on the tape, so this reduces the tape workload.
     */
    const struct dss_sort_item sort_items[] = {
            { .attr = "creation_time", .psql_sort = true },
    };
    enum fs_type source_fs_type = PHO_FS_INVAL;
    struct dss_sort sort = { sort_items, 1 };
    struct pho_io_descr iod_source = {0};
    struct pho_io_descr iod_target = {0};
    struct string_array empty_tags = {0};
    struct pho_ext_loc loc_source = {0};
    struct pho_ext_loc loc_target = {0};
    struct io_adapter_module *ioa = {0};
    struct pho_id target_medium = {0};
    struct pho_id *avoid_media = NULL;
    struct string_array *ptr_tags;
    struct extent *ext_res = NULL;
    struct string_array src_tags;
    GArray *new_ext_uuids = NULL;
    ssize_t target_spc_free = 0;
    struct pho_id allocated;
    int ext_cnt_done = 0;
    ssize_t total_size;
    ssize_t target_spc_needed;
    ssize_t source_phys_spc_used;
    int avoid_cnt = 0;
    int ext_cnt;
    int rc;

    if (source->family != PHO_RSC_TAPE)
        LOG_RETURN(-ENOTSUP, "Repack operation is only available for tapes");

    /* Ignoring the layout check is only allowed with an explicit target:
     * without one, the LRS could select a medium holding a copy of the
     * data of the source medium.
     */
    if (ignore_layout_check && target == NULL)
        LOG_RETURN(-EINVAL, "Ignoring the layout check is only possible "
                            "with an explicit target medium");

    /* Resolve the target tape requested by the caller, if any, and check
     * that it is a usable write target: its library is needed to target it
     * in the write allocation, and resolving it now allows failing before
     * any operation is performed.
     */
    if (target != NULL) {
        rc = _resolve_target_medium(adm, source, target, &target_medium,
                                    &target_spc_free);
        if (rc)
            return rc;
    }

    /* Invoke garbage collector for source tape */
    rc = dss_update_gc_for_tape(&adm->dss, source);
    if (rc)
        return rc;

    /* Determine total size of live objects */
    rc = get_extents_from_medium(adm, source, &ext_res, &ext_cnt, true,
                                 &sort);
    if (rc)
        return rc;

    rc = _retrieve_source_info(adm, source, &src_tags,
                               source_fs_type == PHO_FS_INVAL ?
                                  &source_fs_type : NULL,
                               &source_phys_spc_used);
    if (rc)
        LOG_GOTO(free_ext, rc,
                 "Failed to retrieve info for "FMT_PHO_ID, PHO_ID(*source));

    total_size = _sum_extent_size(ext_res, ext_cnt);
    if (total_size == 0)
        goto format;

    /* Cap the space required on the target medium by the physical space
     * the data occupies on the source medium: the logical size of the
     * extents can exceed it on a filesystem compressing the data, while
     * the target only needs room for the physical payload.
     */
    target_spc_needed = min(total_size, source_phys_spc_used);

    if (target != NULL && target_spc_free <= target_spc_needed)
        LOG_GOTO(free_tags, rc = -ENOSPC,
                 "Target tape '%s' does not have enough free space (%zd) to "
                 "hold the data of the repacked tape (%zd)", target->name,
                 target_spc_free, target_spc_needed);

    rc = _repack_target_check(adm, source, target, &target_medium, empty,
                              ignore_layout_check, &avoid_media, &avoid_cnt);
    if (rc)
        goto free_tags;

    /* Prepare read allocation */
    rc = _get_source_medium(adm, source, &loc_source, &iod_source, &ioa,
                            &source_fs_type);
    if (rc)
        goto free_tags;

    /* Prepare write allocation */
    if (target != NULL)
        /* Use no tags: the target medium is explicitly requested */
        ptr_tags = &empty_tags;
    else if (tags->count == 0)
        /* Use the same tags as the source medium */
        ptr_tags = &src_tags;
    else if (tags->count == 1 && strcmp(tags->strings[0], "") == 0)
        /* Use no tags */
        ptr_tags = &empty_tags;
    else
        /* Use the tags specified */
        ptr_tags = tags;

    rc = _get_target_medium(adm, source, target_spc_needed, ptr_tags, empty,
                            target != NULL ? &target_medium : NULL,
                            target != NULL ? NULL : avoid_media,
                            target != NULL ? 0 : avoid_cnt,
                            &loc_target, &iod_target, &allocated);
    if (rc) {
        free(loc_source.root_path);

        _send_and_recv_release(adm, source, &iod_source, 3, NULL, NULL, 0, 0);
        goto free_tags;
    }

    new_ext_uuids = g_array_new(FALSE, TRUE, sizeof(ext_res[0].uuid));

    rc = _repack_copy_extents(adm, ioa, ext_res, ext_cnt, &iod_source,
                              &iod_target, &allocated, new_ext_uuids,
                              &ext_cnt_done);
    free(loc_target.root_path);
    free(loc_source.root_path);

    if (rc)
        LOG_GOTO(free_ext, rc, "Error encountered, repack is interrupted");

    rc = _send_and_recv_release(adm, source, &iod_source, 3,
                                &allocated, &iod_target,
                                ext_cnt_done == ext_cnt ?
                                    total_size :
                                    _sum_extent_size(ext_res, ext_cnt_done),
                                ext_cnt_done);
    if (rc)
        LOG_GOTO(free_ext, rc, "Failed to send/receive release");

    rc = _repack_migrate_extents(adm, ext_res, ext_cnt_done, new_ext_uuids);
    if (rc)
        goto free_ext;

    g_array_free(new_ext_uuids, TRUE);
    new_ext_uuids = NULL;

format:
    rc = phobos_admin_format(adm, source, 1, 1, source_fs_type, true, true);
    if (rc)
        LOG_GOTO(free_ext, rc,
                 "Failed to format "FMT_PHO_ID, PHO_ID(*source));

    rc = _clean_database_following_format(adm, source);

free_ext:
    if (new_ext_uuids) {
        rc = dss_update_extent_state(&adm->dss,
                                     (const char **)new_ext_uuids->data,
                                     (int)new_ext_uuids->len,
                                     PHO_EXT_ST_ORPHAN);
        g_array_free(new_ext_uuids, TRUE);
        if (rc)
            pho_error(rc, "Failed to update state of new extents to orphan");
    }

free_tags:
    free(avoid_media);
    string_array_free(&src_tags);
    dss_res_free(ext_res, ext_cnt);

    return rc;
}
