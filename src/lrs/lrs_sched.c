/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2022 CEA/DAM.
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
 * \brief  Phobos Local Resource Scheduler (LRS)
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "lrs_cfg.h"
#include "lrs_sched.h"
#include "pho_common.h"
#include "pho_dss.h"
#include "pho_io.h"
#include "pho_ldm.h"
#include "pho_type_utils.h"
#include "lrs_device.h"

#include <assert.h>
#include <glib.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <jansson.h>

#define TAPE_TYPE_SECTION_CFG "tape_type \"%s\""
#define MODELS_CFG_PARAM "models"
#define DRIVE_RW_CFG_PARAM "drive_rw"
#define DRIVE_TYPE_SECTION_CFG "drive_type \"%s\""

enum sched_operation {
    LRS_OP_NONE = 0,
    LRS_OP_READ,
    LRS_OP_WRITE,
    LRS_OP_FORMAT,
};

static int format_media_init(struct format_media *format_media)
{
    int rc;

    rc = pthread_mutex_init(&format_media->mutex, NULL);
    if (rc)
        LOG_RETURN(rc, "Error on initializing format_media mutex");

    format_media->media_name = g_hash_table_new(g_str_hash, g_str_equal);
    return 0;
}

static void format_media_clean(struct format_media *format_media)
{
    int rc;

    rc = pthread_mutex_destroy(&format_media->mutex);
    if (rc)
        pho_error(rc, "Error on destroying format_media mutex");

    if (format_media->media_name) {
        g_hash_table_unref(format_media->media_name);
        format_media->media_name = NULL;
    }
}

/**
 * Add a medium to the format list if not already present
 *
 * @return true if medium is added, false if medium is already present
 */
static bool format_medium_add(struct format_media *format_media,
                              struct media_info *medium)
{
    pthread_mutex_lock(&format_media->mutex);
    if (g_hash_table_contains(format_media->media_name, medium->rsc.id.name)) {
        pthread_mutex_unlock(&format_media->mutex);
        return false;
    }

    g_hash_table_add(format_media->media_name, medium->rsc.id.name);
    pthread_mutex_unlock(&format_media->mutex);
    return true;
}

void format_medium_remove(struct format_media *format_media,
                          struct media_info *medium)
{
    pthread_mutex_lock(&format_media->mutex);
    g_hash_table_remove(format_media->media_name, medium->rsc.id.name);
    pthread_mutex_unlock(&format_media->mutex);
}

/**
 * Build a mount path for the given identifier.
 * @param[in] id    Unique drive identified on the host.
 * The result must be released by the caller using free(3).
 */
static char *mount_point(const char *id)
{
    const char  *mnt_cfg;
    char        *mnt_out;

    mnt_cfg = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, mount_prefix);
    if (mnt_cfg == NULL)
        return NULL;

    /* mount the device as PHO_MNT_PREFIX<id> */
    if (asprintf(&mnt_out, "%s%s", mnt_cfg, id) < 0)
        return NULL;

    return mnt_out;
}

void sched_req_free(void *reqc)
{
    struct req_container *cont = (struct req_container *)reqc;

    if (!cont)
        return;

    if (cont->req) {
        /* this function frees request specific memory, therefore needs to check
         * the request type internally and dereferences the cont->req
         */
        destroy_container_params(cont);
        pho_srl_request_free(cont->req, true);
    }

    pthread_mutex_destroy(&cont->mutex);
    free(cont);
}

/** check that device info from DB is consistent with actual status */
static int check_dev_info(const struct lrs_dev *dev)
{
    ENTRY;

    if (dev->ld_dss_dev_info->rsc.model == NULL
        || dev->ld_sys_dev_state.lds_model == NULL) {
        if (dev->ld_dss_dev_info->rsc.model != dev->ld_sys_dev_state.lds_model)
            LOG_RETURN(-EINVAL, "%s: missing or unexpected device model",
                       dev->ld_dev_path);
        else
            pho_debug("%s: no device model is set", dev->ld_dev_path);

    } else if (cmp_trimmed_strings(dev->ld_dss_dev_info->rsc.model,
                                   dev->ld_sys_dev_state.lds_model)) {
        LOG_RETURN(-EINVAL, "%s: configured device model '%s' differs from "
                   "actual device model '%s'",
                   dev->ld_dev_path,
                   dev->ld_dss_dev_info->rsc.model,
                   dev->ld_sys_dev_state.lds_model);
    }

    if (dev->ld_sys_dev_state.lds_serial == NULL) {
        if (dev->ld_dss_dev_info->rsc.id.name !=
            dev->ld_sys_dev_state.lds_serial)
            LOG_RETURN(-EINVAL, "%s: missing or unexpected device serial",
                       dev->ld_dev_path);
        else
            pho_debug("%s: no device serial is set", dev->ld_dev_path);
    } else if (strcmp(dev->ld_dss_dev_info->rsc.id.name,
                      dev->ld_sys_dev_state.lds_serial) != 0) {
        LOG_RETURN(-EINVAL, "%s: configured device serial '%s' differs from "
                   "actual device serial '%s'",
                   dev->ld_dev_path,
                   dev->ld_dss_dev_info->rsc.id.name,
                   dev->ld_sys_dev_state.lds_serial);
    }

    return 0;
}

/**
 * Unlock a resource device at DSS level and clean the corresponding lock
 *
 * @param[in]   sched   current scheduler
 * @param[in]   type    DSS type of the resource to release
 * @param[in]   item    Resource to release
 * @param[out]  lock    lock to clean
 */
static int sched_resource_release(struct lrs_sched *sched, enum dss_type type,
                                  void *item, struct pho_lock *lock)
{
    int rc;

    ENTRY;

    rc = dss_unlock(&sched->dss, type, item, 1, false);
    if (rc)
        LOG_RETURN(rc, "Cannot unlock a resource");

    pho_lock_clean(lock);
    return 0;
}

static int sched_medium_release(struct lrs_sched *sched,
                                struct media_info *medium)
{
    int rc;

    rc = sched_resource_release(sched, DSS_MEDIA, medium, &medium->lock);
    if (rc)
        pho_error(rc,
                  "Error when releasing medium '%s' with current lock "
                  "(hostname %s, owner %d)", medium->rsc.id.name,
                  medium->lock.hostname, medium->lock.owner);

    pho_debug("unlock: medium %s\n", medium->rsc.id.name);

    return rc;
}

/**
 * Lock the corresponding item into the global DSS and update the local lock
 *
 * @param[in]       dss     DSS handle
 * @param[in]       type    DSS type of the item
 * @param[in]       item    item to lock
 * @param[in, out]  lock    already allocated lock to update
 */
static int take_and_update_lock(struct dss_handle *dss, enum dss_type type,
                                void *item, struct pho_lock *lock)
{
    int rc2;
    int rc;

    pho_lock_clean(lock);
    rc = dss_lock(dss, type, item, 1);
    if (rc)
        pho_error(rc, "Unable to get lock on item for refresh");

    pho_debug("lock: %s %s\n", dss_type2str(type),
              type == DSS_DEVICE ?
              ((struct dev_info *)item)->rsc.id.name :
              type == DSS_MEDIA ?
              ((struct media_info *)item)->rsc.id.name :
              "???");

    /* update lock values */
    rc2 = dss_lock_status(dss, type, item, 1, lock);
    if (rc2) {
        pho_error(rc2, "Unable to get status of new lock while refreshing");
        /* try to unlock before exiting */
        if (rc == 0) {
            dss_unlock(dss, type, item, 1, false);
            rc = rc2;
        }

        /* put a wrong lock value */
        lock->hostname = NULL;
        lock->owner = -1;
        lock->timestamp.tv_sec = 0;
        lock->timestamp.tv_usec = 0;
    }

    return rc;
}

/**
 * If lock->owner is different from sched->lock_owner, renew the lock with
 * the current owner (PID).
 */
static int check_renew_owner(struct lrs_sched *sched, enum dss_type type,
                             void *item, struct pho_lock *lock)
{
    int rc;

    if (lock->owner != sched->lock_owner) {
        pho_warn("'%s' is already locked by owner %d, owner %d will "
                 "take ownership of this device",
                 dss_type_names[type], lock->owner, sched->lock_owner);

        /**
         * Unlocking here is dangerous if there is another process than the
         * LRS on the same node that also acquires locks. If it becomes the case
         * we have to warn and return an error and we must not take the
         * ownership of this resource again.
         */
        /* unlock previous owner */
        rc = dss_unlock(&sched->dss, type, item, 1, true);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to clear previous lock (hostname: %s, owner "
                       " %d) on item",
                       lock->hostname, lock->owner);

        /* get the lock again */
        rc = take_and_update_lock(&sched->dss, type, item, lock);
        if (rc)
            LOG_RETURN(rc, "Unable to get and refresh lock");
    }

    return 0;
}

/**
 * First, check that lock->hostname is the same as sched->lock_hostname. If not,
 * -EALREADY is returned.
 *
 * Then, if lock->owner is different from sched->lock_owner, renew the lock with
 * the current owner (PID) by calling check_renew_owner.
 */
static int check_renew_lock(struct lrs_sched *sched, enum dss_type type,
                            void *item, struct pho_lock *lock)
{
    if (strcmp(lock->hostname, sched->lock_hostname)) {
        pho_warn("Resource already locked by host %s instead of %s",
                 lock->hostname, sched->lock_hostname);
        return -EALREADY;
    }

    return check_renew_owner(sched, type, item, lock);
}

int check_and_take_device_lock(struct lrs_sched *sched,
                               struct dev_info *dev)
{
    int rc;

    if (dev->lock.hostname) {
        rc = check_renew_lock(sched, DSS_DEVICE, dev, &dev->lock);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to check and renew lock of one of our devices "
                       "'%s'", dev->rsc.id.name);
    } else {
        rc = take_and_update_lock(&sched->dss, DSS_DEVICE, dev, &dev->lock);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to acquire and update lock on device '%s'",
                       dev->rsc.id.name);
    }

    return 0;
}

/**
 * Retrieve media info from DSS for the given ID.
 * @param pmedia[out] returned pointer to a media_info structure
 *                    allocated by this function.
 * @param id[in]      ID of the media.
 */
static int sched_fill_media_info(struct lrs_sched *sched,
                                 struct media_info **pmedia,
                                 const struct pho_id *id)
{
    struct dss_handle   *dss = &sched->dss;
    struct media_info   *media_res = NULL;
    struct dss_filter    filter;
    int                  mcnt = 0;
    int                  rc;

    if (id == NULL || pmedia == NULL)
        return -EINVAL;

    pho_debug("Retrieving media info for %s '%s'",
              rsc_family2str(id->family), id->name);

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          "  {\"DSS::MDA::id\": \"%s\"}"
                          "]}", rsc_family2str(id->family), id->name);
    if (rc)
        return rc;

    /* get media info from DB */
    rc = dss_media_get(dss, &filter, &media_res, &mcnt);
    if (rc)
        GOTO(out_nores, rc);

    if (mcnt == 0) {
        pho_info("No media found matching %s '%s'",
                 rsc_family2str(id->family), id->name);
        GOTO(out_free, rc = -ENXIO);
    } else if (mcnt > 1) {
        LOG_GOTO(out_free, rc = -EINVAL,
                 "Too many media found matching id '%s'", id->name);
    }

    media_info_free(*pmedia);
    *pmedia = media_info_dup(media_res);
    if (!*pmedia)
        LOG_GOTO(out_free, rc = -ENOMEM, "Couldn't duplicate media info");

    if ((*pmedia)->lock.hostname != NULL) {
        rc = check_renew_lock(sched, DSS_MEDIA, *pmedia, &(*pmedia)->lock);
        if (rc == -EALREADY) {
            LOG_GOTO(out_free, rc,
                     "Media '%s' is locked by (hostname: %s, owner: %d)",
                     id->name, (*pmedia)->lock.hostname, (*pmedia)->lock.owner);
        } else if (rc) {
            LOG_GOTO(out_free, rc,
                     "Error while checking media '%s' locked with hostname "
                     "'%s' and owner '%d'",
                     id->name, (*pmedia)->lock.hostname, (*pmedia)->lock.owner);
        }
    }

    pho_debug("%s: spc_free=%zd",
              (*pmedia)->rsc.id.name, (*pmedia)->stats.phys_spc_free);

    rc = 0;

out_free:
    dss_res_free(media_res, mcnt);
out_nores:
    dss_filter_free(&filter);
    return rc;
}

/**
 * Retrieve device information from system and complementary info from DB.
 * - check DB device info is consistent with mtx output.
 * - get operationnal status from system (loaded or not).
 * - for loaded drives, the mounted volume + LTFS mount point, if mounted.
 * - get media information from DB for loaded drives.
 *
 * @param[in]  dss  handle to dss connection.
 * @param[in]  lib  library handler for tape devices.
 * @param[out] dev  lrs_dev structure filled with all needed information.
 */
static int sched_fill_dev_info(struct lrs_sched *sched, struct lib_adapter *lib,
                               struct lrs_dev *dev)
{
    struct dev_adapter deva;
    struct dev_info   *devi;
    int                rc;

    ENTRY;

    if (dev == NULL)
        return -EINVAL;

    devi = dev->ld_dss_dev_info;

    media_info_free(dev->ld_dss_media_info);
    dev->ld_dss_media_info = NULL;
    dev->ld_op_status = PHO_DEV_OP_ST_EMPTY;

    rc = get_dev_adapter(devi->rsc.id.family, &deva);
    if (rc)
        return rc;

    /* get path for the given serial */
    rc = ldm_dev_lookup(&deva, devi->rsc.id.name, dev->ld_dev_path,
                        sizeof(dev->ld_dev_path));
    if (rc) {
        pho_debug("Device lookup failed: serial '%s'", devi->rsc.id.name);
        return rc;
    }

    /* now query device by path */
    ldm_dev_state_fini(&dev->ld_sys_dev_state);
    rc = ldm_dev_query(&deva, dev->ld_dev_path, &dev->ld_sys_dev_state);
    if (rc) {
        pho_debug("Failed to query device '%s'", dev->ld_dev_path);
        return rc;
    }

    /* compare returned device info with info from DB */
    rc = check_dev_info(dev);
    if (rc)
        return rc;

    /* Query the library about the drive location and whether it contains
     * a media.
     */
    rc = ldm_lib_drive_lookup(lib, devi->rsc.id.name, &dev->ld_lib_dev_info);
    if (rc) {
        pho_debug("Failed to query the library about device '%s'",
                  devi->rsc.id.name);
        return rc;
    }

    if (dev->ld_lib_dev_info.ldi_full) {
        struct pho_id *medium_id;
        struct fs_adapter fsa;

        dev->ld_op_status = PHO_DEV_OP_ST_LOADED;
        medium_id = &dev->ld_lib_dev_info.ldi_medium_id;

        pho_debug("Device '%s' (S/N '%s') contains medium '%s'",
                  dev->ld_dev_path, devi->rsc.id.name, medium_id->name);

        /* get media info for loaded drives */
        rc = sched_fill_media_info(sched, &dev->ld_dss_media_info, medium_id);

        if (rc) {
            if (rc == -ENXIO)
                pho_error(rc,
                          "Device '%s' (S/N '%s') contains medium '%s', but "
                          "this medium cannot be found", dev->ld_dev_path,
                          devi->rsc.id.name, medium_id->name);

            if (rc == -EALREADY)
                pho_error(rc,
                          "Device '%s' (S/N '%s') is owned by host %s but "
                          "contains medium '%s' which is locked by an other "
                          "hostname %s", dev->ld_dev_path, devi->rsc.id.name,
                           devi->host, medium_id->name,
                           dev->ld_dss_media_info->lock.hostname);

            return rc;
        }

        /* get lock for loaded media */
        if (!dev->ld_dss_media_info->lock.hostname) {
            rc = take_and_update_lock(&sched->dss, DSS_MEDIA,
                                      dev->ld_dss_media_info,
                                      &dev->ld_dss_media_info->lock);
            if (rc)
                LOG_RETURN(rc,
                           "Unable to lock the media '%s' loaded in an owned "
                           "device '%s'", dev->ld_dss_media_info->rsc.id.name,
                           dev->ld_dev_path);
        }

        /* See if the device is currently mounted */
        rc = get_fs_adapter(dev->ld_dss_media_info->fs.type, &fsa);
        if (rc)
            return rc;

        /* If device is loaded, check if it is mounted as a filesystem */
        rc = ldm_fs_mounted(&fsa, dev->ld_dev_path, dev->ld_mnt_path,
                            sizeof(dev->ld_mnt_path));

        if (rc == 0) {
            pho_debug("Discovered mounted filesystem at '%s'",
                      dev->ld_mnt_path);
            dev->ld_op_status = PHO_DEV_OP_ST_MOUNTED;
        } else if (rc == -ENOENT) {
            /* not mounted, not an error */
            rc = 0;
        } else {
            LOG_RETURN(rc, "Cannot determine if device '%s' is mounted",
                       dev->ld_dev_path);
        }
    } else {
        dev->ld_op_status = PHO_DEV_OP_ST_EMPTY;
    }

    pho_debug("Drive '%s' is '%s'", dev->ld_dev_path,
              op_status2str(dev->ld_op_status));

    return rc;
}

/**
 * Load device states into memory.
 * Do nothing if device status is already loaded.
 */
static int sched_load_dev_state(struct lrs_sched *sched)
{
    bool                clean_devices = false;
    struct lib_adapter  lib;
    int                 rc;
    int                 i;

    ENTRY;

    if (sched->devices.ldh_devices->len == 0) {
        pho_verb("Try to load state of an empty list of devices");
        return -ENXIO;
    }

    /* get a handle to the library to query it */
    rc = wrap_lib_open(sched->family, &lib);
    if (rc)
        LOG_RETURN(rc, "Error while loading devices when opening library");

    for (i = 0 ; i < sched->devices.ldh_devices->len; i++) {
        struct lrs_dev *dev;

        dev = lrs_dev_hdl_get(&sched->devices, i);

        MUTEX_LOCK(&dev->ld_mutex);
        rc = sched_fill_dev_info(sched, &lib, dev);
        if (rc) {
            pho_error(rc,
                      "Fail to init device '%s', stopping corresponding device "
                      "thread", dev->ld_dev_path);
            dev_thread_signal_stop_on_error(dev, rc);
        } else {
            clean_devices = true;
        }
        MUTEX_UNLOCK(&dev->ld_mutex);
    }

    /* close handle to the library */
    rc = ldm_lib_close(&lib);
    if (rc)
        LOG_RETURN(rc,
                   "Error while closing the library handle after loading "
                   "device state");

    if (!clean_devices)
        LOG_RETURN(-ENXIO, "No functional device found");

    return 0;
}

/**
 * Unlocks all devices that were locked by a previous instance on this host and
 * that it doesn't own anymore.
 *
 * @param   sched       Scheduler handle.
 * @return              0 on success,
 *                      first encountered negative posix error on failure.
 */
static int sched_clean_device_locks(struct lrs_sched *sched)
{
    int rc;

    ENTRY;

    rc = dss_lock_device_clean(&sched->dss, rsc_family_names[sched->family],
                               sched->lock_hostname, sched->lock_owner);
    if (rc)
        pho_error(rc, "Failed to clean device locks");

    return rc;
}

/**
 * Unlocks all media that were locked by a previous instance on this host and
 * that are not loaded anymore in a device locked by this host.
 *
 * @param   sched       Scheduler handle.
 * @return              0 on success,
 *                      first encountered negative posix error on failure.
 */
static int sched_clean_medium_locks(struct lrs_sched *sched)
{
    struct media_info *media = NULL;
    int cnt = 0;
    int rc;
    int i;

    ENTRY;

    media = malloc(sched->devices.ldh_devices->len * sizeof(*media));
    if (!media)
        LOG_RETURN(-errno, "Failed to allocate media list");

    for (i = 0; i < sched->devices.ldh_devices->len; i++) {
        struct media_info *mda;
        struct lrs_dev *dev;

        dev = lrs_dev_hdl_get(&sched->devices, i);
        if (dev->ld_device_thread.ld_running) {
            mda = dev->ld_dss_media_info;

            if (mda)
                media[cnt++] = *mda;
        }
    }

    rc = dss_lock_media_clean(&sched->dss, media, cnt,
                              sched->lock_hostname, sched->lock_owner);
    if (rc)
        pho_error(rc, "Failed to clean media locks");

    free(media);
    return rc;
}

int sched_init(struct lrs_sched *sched, enum rsc_family family,
               struct tsqueue *resp_queue)
{
    int rc;

    sched->family = family;

    rc = format_media_init(&sched->ongoing_format);
    if (rc)
        LOG_RETURN(rc, "Failed to init sched format media");

    rc = lrs_dev_hdl_init(&sched->devices, family);
    if (rc)
        LOG_GOTO(err_format_media, rc, "Failed to initialize device handle");

    rc = fill_host_owner(&sched->lock_hostname, &sched->lock_owner);
    if (rc)
        LOG_GOTO(err_hdl_fini, rc, "Failed to get hostname and PID");

    /* Connect to the DSS */
    rc = dss_init(&sched->dss);
    if (rc)
        LOG_GOTO(err_hdl_fini, rc, "Failed to init sched dss handle");

    rc = tsqueue_init(&sched->req_queue);
    if (rc)
        LOG_GOTO(err_dss_fini, rc, "Failed to init sched req_queue");

    sched->response_queue = resp_queue;

    /* Load devices from DSS -- not critical if no device is found */
    lrs_dev_hdl_load(sched, &sched->devices);

    /* Load the device state -- not critical if no device is found */
    sched_load_dev_state(sched);

    rc = sched_clean_device_locks(sched);
    if (rc)
        goto err_sched_fini;

    rc = sched_clean_medium_locks(sched);
    if (rc)
        goto err_sched_fini;

    return 0;

err_sched_fini:
    sched_fini(sched);
    return rc;

err_dss_fini:
    dss_fini(&sched->dss);
err_hdl_fini:
    lrs_dev_hdl_fini(&sched->devices);
err_format_media:
    format_media_clean(&sched->ongoing_format);
    return rc;
}

int prepare_error(struct resp_container *resp_cont, int req_rc,
                  const struct req_container *req_cont)
{
    int rc;

    resp_cont->socket_id = req_cont->socket_id;
    rc = pho_srl_response_error_alloc(resp_cont->resp);
    if (rc)
        LOG_RETURN(rc, "Failed to allocate response");

    resp_cont->resp->error->rc = req_rc;

    resp_cont->resp->req_id = req_cont->req->id;
    if (pho_request_is_write(req_cont->req))
        resp_cont->resp->error->req_kind = PHO_REQUEST_KIND__RQ_WRITE;
    else if (pho_request_is_read(req_cont->req))
        resp_cont->resp->error->req_kind = PHO_REQUEST_KIND__RQ_READ;
    else if (pho_request_is_release(req_cont->req))
        resp_cont->resp->error->req_kind = PHO_REQUEST_KIND__RQ_RELEASE;
    else if (pho_request_is_format(req_cont->req))
        resp_cont->resp->error->req_kind = PHO_REQUEST_KIND__RQ_FORMAT;
    else if (pho_request_is_notify(req_cont->req))
        resp_cont->resp->error->req_kind = PHO_REQUEST_KIND__RQ_NOTIFY;

    return 0;
}

int queue_error_response(struct tsqueue *response_queue, int req_rc,
                         struct req_container *reqc)
{
    struct resp_container *resp_cont;
    int rc;

    resp_cont = malloc(sizeof(*resp_cont));
    if (!resp_cont)
        LOG_RETURN(-ENOMEM, "Unable to allocate resp_cont");

    resp_cont->resp = malloc(sizeof(*resp_cont->resp));
    if (!resp_cont->resp)
        LOG_GOTO(clean_resp_cont, rc = -ENOMEM,
                 "Unable to allocate resp_cont->resp");

    rc = prepare_error(resp_cont, req_rc, reqc);
    if (rc)
        goto clean;

    tsqueue_push(response_queue, resp_cont);

    return 0;

clean:
    free(resp_cont->resp);
clean_resp_cont:
    free(resp_cont);
    return rc;
}

/**
 * Unmount the filesystem of a 'mounted' device
 *
 * sched_umount must be called with:
 * - dev->ld_op_status set to PHO_DEV_OP_ST_MOUNTED, a mounted
 *   dev->ld_dss_media_info
 * - a global DSS lock on dev
 * - a global DSS lock on dev->ld_dss_media_info
 *
 * On error, signal the error to the device thread.
 */
static int sched_umount(struct lrs_sched *sched, struct lrs_dev *dev)
{
    struct fs_adapter fsa;
    int rc, rc2;

    ENTRY;

    pho_info("umount: device '%s' mounted at '%s'",
             dev->ld_dev_path, dev->ld_mnt_path);

    rc = get_fs_adapter(dev->ld_dss_media_info->fs.type, &fsa);
    if (rc)
        LOG_GOTO(out, rc,
                 "Unable to get fs adapter '%s' to unmount medium '%s' from "
                 "device '%s'", fs_type_names[dev->ld_dss_media_info->fs.type],
                 dev->ld_dss_media_info->rsc.id.name, dev->ld_dev_path);

    rc = ldm_fs_umount(&fsa, dev->ld_dev_path, dev->ld_mnt_path);
    rc2 = clean_tosync_array(dev, rc);
    if (rc)
        LOG_GOTO(out, rc, "Failed to unmount device '%s' mounted at '%s'",
                 dev->ld_dev_path, dev->ld_mnt_path);

    /* update device state and unset mount path */
    dev->ld_op_status = PHO_DEV_OP_ST_LOADED;
    dev->ld_mnt_path[0] = '\0';

    if (rc2)
        LOG_GOTO(out, rc = rc2,
                 "Failed to clean tosync array after having unmounted device "
                 "'%s' mounted at '%s'",
                 dev->ld_dev_path, dev->ld_mnt_path);

out:
    if (rc)
        dev_thread_signal_stop_on_error(dev, rc);

    return rc;
}

/**
 * Unload, unlock and free a media from a drive and set drive's ld_op_status to
 * PHO_DEV_OP_ST_EMPTY
 *
 * Must be called with:
 * - dev->ld_op_status set to PHO_DEV_OP_ST_LOADED and loaded
 *   dev->ld_dss_media_info
 * - a global DSS lock on dev
 * - a global DSS lock on dev->ld_dss_media_info
 *
 * On error, signal the error to the device thread.
 */
static int sched_unload_medium(struct lrs_sched *sched, struct lrs_dev *dev)
{
    /* let the library select the target location */
    struct lib_item_addr    free_slot = { .lia_type = MED_LOC_UNKNOWN };
    struct lib_adapter      lib;
    int                     rc2;
    int                     rc;

    ENTRY;

    pho_verb("Unloading '%s' from '%s'", dev->ld_dss_media_info->rsc.id.name,
             dev->ld_dev_path);

    rc = wrap_lib_open(dev->ld_dss_dev_info->rsc.id.family, &lib);
    if (rc)
        LOG_GOTO(out, rc,
                 "Unable to open lib '%s' to unload medium '%s' from device "
                 "'%s'", rsc_family_names[dev->ld_dss_dev_info->rsc.id.family],
                 dev->ld_dss_media_info->rsc.id.name, dev->ld_dev_path);

    rc = ldm_lib_media_move(&lib, &dev->ld_lib_dev_info.ldi_addr, &free_slot);
    if (rc != 0)
        /* Set operationnal failure state on this drive. It is incomplete since
         * the error can originate from a defective tape too...
         *  - consider marking both as failed.
         *  - consider maintaining lists of errors to diagnose and decide who to
         *    exclude from the cool game.
         */
        LOG_GOTO(out_close, rc, "Media move failed");

    dev->ld_op_status = PHO_DEV_OP_ST_EMPTY;

out_close:
    rc2 = ldm_lib_close(&lib);
    if (rc2)
        rc = rc ? : rc2;

out:
    rc2 = sched_medium_release(sched, dev->ld_dss_media_info);
    if (rc2)
        rc = rc ? : rc2;

    media_info_free(dev->ld_dss_media_info);
    dev->ld_dss_media_info = NULL;

    if (rc)
        dev_thread_signal_stop_on_error(dev, rc);

    return rc;
}

void sched_resp_free(void *_respc)
{
    struct resp_container *respc = (struct resp_container *)_respc;

    if (pho_response_is_write(respc->resp) ||
        pho_response_is_read(respc->resp))
        free(respc->devices);

    pho_srl_response_free(respc->resp, false);
    free(respc->resp);
}

void sched_fini(struct lrs_sched *sched)
{
    if (sched == NULL)
        return;

    lrs_dev_hdl_clear(&sched->devices);
    lrs_dev_hdl_fini(&sched->devices);
    dss_fini(&sched->dss);
    tsqueue_destroy(&sched->req_queue, sched_req_free);
    format_media_clean(&sched->ongoing_format);
}

bool sched_has_running_devices(struct lrs_sched *sched)
{
    int i;

    for (i = 0; i < sched->devices.ldh_devices->len; i++) {
        struct lrs_dev *dev;

        dev = lrs_dev_hdl_get(&sched->devices, i);
        if (dev->ld_ongoing_io)
            return true;
    }

    return false;
}

/**
 * Build a filter string fragment to filter on a given tag set. The returned
 * string is allocated with malloc. NULL is returned when ENOMEM is encountered.
 *
 * The returned string looks like the following:
 * {"$AND": [{"DSS:MDA::tags": "tag1"}]}
 */
static char *build_tag_filter(const struct tags *tags)
{
    json_t *and_filter = NULL;
    json_t *tag_filters = NULL;
    char   *tag_filter_json = NULL;
    size_t  i;

    /* Build a json array to properly format tag related DSS filter */
    tag_filters = json_array();
    if (!tag_filters)
        return NULL;

    /* Build and append one filter per tag */
    for (i = 0; i < tags->n_tags; i++) {
        json_t *tag_flt;
        json_t *xjson;

        tag_flt = json_object();
        if (!tag_flt)
            GOTO(out, -ENOMEM);

        xjson = json_object();
        if (!xjson) {
            json_decref(tag_flt);
            GOTO(out, -ENOMEM);
        }

        if (json_object_set_new(tag_flt, "DSS::MDA::tags",
                                json_string(tags->tags[i]))) {
            json_decref(tag_flt);
            json_decref(xjson);
            GOTO(out, -ENOMEM);
        }

        if (json_object_set_new(xjson, "$XJSON", tag_flt)) {
            json_decref(xjson);
            GOTO(out, -ENOMEM);
        }

        if (json_array_append_new(tag_filters, xjson))
            GOTO(out, -ENOMEM);
    }

    and_filter = json_object();
    if (!and_filter)
        GOTO(out, -ENOMEM);

    /* Do not use the _new function and decref inconditionnaly later */
    if (json_object_set(and_filter, "$AND", tag_filters))
        GOTO(out, -ENOMEM);

    /* Convert to string for formatting */
    tag_filter_json = json_dumps(tag_filters, 0);

out:
    json_decref(tag_filters);

    /* json_decref(NULL) is safe but not documented */
    if (and_filter)
        json_decref(and_filter);

    return tag_filter_json;
}

static bool medium_in_devices(const struct media_info *medium,
                              struct lrs_dev **devs, size_t n_dev)
{
    size_t i;

    for (i = 0; i < n_dev; i++) {
        if (devs[i]->ld_dss_media_info == NULL)
            continue;
        if (pho_id_equal(&medium->rsc.id, &devs[i]->ld_dss_media_info->rsc.id))
            return true;
    }

    return false;
}

static struct lrs_dev *search_loaded_media(struct lrs_sched *sched,
                                           const char *name)
{
    int i;

    ENTRY;

    for (i = 0; i < sched->devices.ldh_devices->len; i++) {
        const char *media_id;
        struct lrs_dev *dev;

        dev = lrs_dev_hdl_get(&sched->devices, i);

        if (dev->ld_op_status != PHO_DEV_OP_ST_MOUNTED &&
            dev->ld_op_status != PHO_DEV_OP_ST_LOADED)
            continue;

        /* The drive may contain a media unknown to phobos, skip it */
        if (dev->ld_dss_media_info == NULL)
            continue;

        media_id = dev->ld_dss_media_info->rsc.id.name;
        if (media_id == NULL) {
            pho_warn("Cannot retrieve media ID from device '%s'",
                     dev->ld_dev_path);
            continue;
        }

        if (!strcmp(name, media_id))
            return dev;
    }

    return NULL;
}

/**
 * Get a suitable medium for a write operation.
 *
 * @param[in]  sched         Current scheduler
 * @param[out] p_media       Selected medium
 * @param[in]  required_size Size of the extent to be written.
 * @param[in]  family        Medium family from which getting the medium
 * @param[in]  tags          Tags used to filter candidate media, the
 *                           selected medium must have all the specified tags.
 * @param[in]  devs          Array of selected devices to write with.
 * @param[in]  n_dev         Nb in devs of already allocated devices with loaded
 *                           and mounted media
 */
static int sched_select_media(struct lrs_sched *sched,
                              struct media_info **p_media, size_t required_size,
                              enum rsc_family family, const struct tags *tags,
                              struct lrs_dev **devs, size_t n_dev)
{
    struct media_info   *pmedia_res = NULL;
    struct media_info   *split_media_best;
    size_t               avail_size;
    struct media_info   *whole_media_best;
    struct media_info   *chosen_media;
    struct dss_filter    filter;
    char                *tag_filter_json = NULL;
    bool                 with_tags = tags != NULL && tags->n_tags > 0;
    int                  mcnt = 0;
    int                  rc;
    int                  i;

    ENTRY;

    if (with_tags) {
        tag_filter_json = build_tag_filter(tags);
        if (!tag_filter_json)
            LOG_GOTO(err_nores, rc = -ENOMEM, "while building tags dss filter");
    }

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          /* Basic criteria */
                          "  {\"DSS::MDA::family\": \"%s\"},"
                          /* Check put media operation flags */
                          "  {\"DSS::MDA::put\": \"t\"},"
                          /* Exclude media locked by admin */
                          "  {\"DSS::MDA::adm_status\": \"%s\"},"
                          "  {\"$NOR\": ["
                               /* Exclude non-formatted media */
                          "    {\"DSS::MDA::fs_status\": \"%s\"},"
                               /* Exclude full media */
                          "    {\"DSS::MDA::fs_status\": \"%s\"}"
                          "  ]}"
                          "  %s%s"
                          "]}",
                          rsc_family2str(family),
                          rsc_adm_status2str(PHO_RSC_ADM_ST_UNLOCKED),
                          /**
                           * @TODO add criteria to limit the maximum number of
                           * data fragments:
                           * vol_free >= required_size/max_fragments
                           * with a configurable max_fragments of 4 for example)
                           */
                          fs_status2str(PHO_FS_STATUS_BLANK),
                          fs_status2str(PHO_FS_STATUS_FULL),
                          with_tags ? ", " : "",
                          with_tags ? tag_filter_json : "");

    free(tag_filter_json);
    if (rc)
        return rc;

    rc = dss_media_get(&sched->dss, &filter, &pmedia_res, &mcnt);
    dss_filter_free(&filter);
    if (rc)
        GOTO(err_nores, rc);

lock_race_retry:
    chosen_media = NULL;
    whole_media_best = NULL;
    split_media_best = NULL;
    avail_size = 0;

    /* get the best fit */
    for (i = 0; i < mcnt; i++) {
        struct media_info *curr = &pmedia_res[i];

        /* exclude medium already booked for this allocation */
        if (medium_in_devices(curr, devs, n_dev))
            continue;

        avail_size += curr->stats.phys_spc_free;

        /* already locked */
        if (curr->lock.hostname != NULL) {
            struct lrs_dev *dev;

            if (check_renew_lock(sched, DSS_MEDIA, curr, &curr->lock))
                /* not locked by myself */
                continue;

            dev = search_loaded_media(sched, curr->rsc.id.name);
            if (dev && (dev->ld_ongoing_io || dev->ld_needs_sync ||
                        !dev->ld_device_thread.ld_running))
                /* locked by myself but already in use */
                continue;
        }

        if (split_media_best == NULL ||
            curr->stats.phys_spc_free > split_media_best->stats.phys_spc_free)
            split_media_best = curr;

        if (curr->stats.phys_spc_free < required_size)
            continue;

        if (whole_media_best == NULL ||
            curr->stats.phys_spc_free < whole_media_best->stats.phys_spc_free)
            whole_media_best = curr;
    }

    if (avail_size < required_size) {
        pho_warn("Available space on media : %zd, required size : %zd",
                 avail_size, required_size);
        GOTO(free_res, rc = -ENOSPC);
    }

    if (whole_media_best != NULL) {
        chosen_media = whole_media_best;
    } else if (split_media_best != NULL) {
        chosen_media = split_media_best;
        pho_info("Split %zd required_size on %zd avail size on %s medium",
                 required_size, chosen_media->stats.phys_spc_free,
                 chosen_media->rsc.id.name);
    } else {
        pho_info("No medium available, wait for one");
        GOTO(free_res, rc = -EAGAIN);
    }

    if (!chosen_media->lock.hostname) {
        pho_debug("Acquiring selected media '%s'", chosen_media->rsc.id.name);
        rc = take_and_update_lock(&sched->dss, DSS_MEDIA, chosen_media,
                                  &chosen_media->lock);
        if (rc) {
            pho_debug("Failed to lock media '%s', looking for another one",
                      chosen_media->rsc.id.name);
            goto lock_race_retry;
        }
    }

    pho_verb("Selected %s '%s': %zd bytes free", rsc_family2str(family),
             chosen_media->rsc.id.name,
             chosen_media->stats.phys_spc_free);

    *p_media = media_info_dup(chosen_media);
    if (*p_media == NULL) {
        sched_medium_release(sched, chosen_media);
        LOG_GOTO(free_res, rc = -ENOMEM,
                 "Unable to duplicate chosen media '%s'",
                 chosen_media->rsc.id.name);
    }

    rc = 0;

free_res:
    dss_res_free(pmedia_res, mcnt);

err_nores:
    return rc;
}

/**
 * Get the value of the configuration parameter that contains
 * the list of drive models for a given drive type.
 * e.g. "LTO6_drive" -> "ULTRIUM-TD6,ULT3580-TD6,..."
 *
 * @return 0 on success, a negative POSIX error code on failure.
 */
static int drive_models_by_type(const char *drive_type, const char **list)
{
    char *section_name;
    int rc;

    /* build drive_type section name */
    rc = asprintf(&section_name, DRIVE_TYPE_SECTION_CFG,
                  drive_type);
    if (rc < 0)
        return -ENOMEM;

    /* get list of drive models */
    rc = pho_cfg_get_val(section_name, MODELS_CFG_PARAM, list);
    if (rc)
        pho_error(rc, "Unable to find parameter "MODELS_CFG_PARAM" in section "
                  "'%s' for drive type '%s'", section_name, drive_type);

    free(section_name);
    return rc;
}

/**
 * Get the value of the configuration parameter that contains
 * the list of write-compatible drives for a given tape model.
 * e.g. "LTO5" -> "LTO5_drive,LTO6_drive"
 *
 * @return 0 on success, a negative POSIX error code on failure.
 */
static int rw_drive_types_for_tape(const char *tape_model, const char **list)
{
    char *section_name;
    int rc;

    /* build tape_type section name */
    rc = asprintf(&section_name, TAPE_TYPE_SECTION_CFG, tape_model);
    if (rc < 0)
        return -ENOMEM;

    /* get list of drive_rw types */
    rc = pho_cfg_get_val(section_name, DRIVE_RW_CFG_PARAM, list);
    if (rc)
        pho_error(rc, "Unable to find parameter "DRIVE_RW_CFG_PARAM
                  " in section '%s' for tape model '%s'",
                  section_name, tape_model);

    free(section_name);
    return rc;
}

/**
 * Search a given item in a coma-separated list.
 *
 * @param[in]  list     Comma-separated list of items.
 * @param[in]  str      Item to find in the list.
 * @param[out] res      true of the string is found, false else.
 *
 * @return 0 on success. A negative POSIX error code on error.
 */
static int search_in_list(const char *list, const char *str, bool *res)
{
    char *parse_list;
    char *item;
    char *saveptr;

    *res = false;

    /* copy input list to parse it */
    parse_list = strdup(list);
    if (parse_list == NULL)
        return -errno;

    /* check if the string is in the list */
    for (item = strtok_r(parse_list, ",", &saveptr);
         item != NULL;
         item = strtok_r(NULL, ",", &saveptr)) {
        if (strcmp(item, str) == 0) {
            *res = true;
            goto out_free;
        }
    }

out_free:
    free(parse_list);
    return 0;
}

/**
 * This function determines if the input drive and tape are compatible.
 *
 * @param[in]  tape  tape to check compatibility
 * @param[in]  drive drive to check compatibility
 * @param[out] res   true if the tape and drive are compatible, else false
 *
 * @return 0 on success, negative error code on failure and res is false
 */
static int tape_drive_compat(const struct media_info *tape,
                             const struct lrs_dev *drive, bool *res)
{
    const char *rw_drives;
    char *parse_rw_drives;
    char *drive_type;
    char *saveptr;
    int rc;

    /* false by default */
    *res = false;

    /** XXX FIXME: this function is called for each drive for the same tape by
     *  the function dev_picker. Each time, we build/allocate same strings and
     *  we parse again the conf. This behaviour is heavy and not optimal.
     */
    rc = rw_drive_types_for_tape(tape->rsc.model, &rw_drives);
    if (rc)
        return rc;

    /* copy the rw_drives list to tokenize it */
    parse_rw_drives = strdup(rw_drives);
    if (parse_rw_drives == NULL)
        return -errno;

    /* For each compatible drive type, get list of associated drive models
     * and search the current drive model in it.
     */
    for (drive_type = strtok_r(parse_rw_drives, ",", &saveptr);
         drive_type != NULL;
         drive_type = strtok_r(NULL, ",", &saveptr)) {
        const char *drive_model_list;

        rc = drive_models_by_type(drive_type, &drive_model_list);
        if (rc)
            goto out_free;

        rc = search_in_list(drive_model_list, drive->ld_dss_dev_info->rsc.model,
                            res);
        if (rc)
            goto out_free;
        /* drive model found: media is compatible */
        if (*res)
            break;
    }

out_free:
    free(parse_rw_drives);
    return rc;
}


/**
 * Device selection policy prototype.
 * @param[in]     required_size required space to perform the write operation.
 * @param[in]     dev_curr      the current device to consider.
 * @param[in,out] dev_selected  the currently selected device.
 * @retval <0 on error
 * @retval 0 to stop searching for a device
 * @retval >0 to check next devices.
 */
typedef int (*device_select_func_t)(size_t required_size,
                                    struct lrs_dev *dev_curr,
                                    struct lrs_dev **dev_selected);

/**
 * Select a device according to a given status and policy function.
 * Returns a device by setting its ld_ongoing_io flag to true.
 *
 * @param dss     DSS handle.
 * @param op_st   Filter devices by the given operational status.
 *                No filtering is op_st is PHO_DEV_OP_ST_UNSPEC.
 * @param select_func    Drive selection function.
 * @param required_size  Required size for the operation.
 * @param media_tags     Mandatory tags for the contained media (for write
 *                       requests only).
 * @param pmedia         Media that should be used by the drive to check
 *                       compatibility (ignored if NULL)
 */
static struct lrs_dev *dev_picker(struct lrs_sched *sched,
                                    enum dev_op_status op_st,
                                    device_select_func_t select_func,
                                    size_t required_size,
                                    const struct tags *media_tags,
                                    struct media_info *pmedia, bool is_write)
{
    struct lrs_dev    *selected = NULL;
    int                  selected_i = -1;
    int                  i;
    int                  rc;

    ENTRY;

    for (i = 0; i < sched->devices.ldh_devices->len; i++) {
        struct lrs_dev *itr = lrs_dev_hdl_get(&sched->devices, i);
        struct lrs_dev *prev = selected;

        if (itr->ld_ongoing_io || itr->ld_needs_sync) {
            pho_debug("Skipping busy device '%s'", itr->ld_dev_path);
            continue;
        }

        if ((itr->ld_op_status == PHO_DEV_OP_ST_FAILED) ||
            (op_st != PHO_DEV_OP_ST_UNSPEC && itr->ld_op_status != op_st)) {
            pho_debug("Skipping device '%s' with incompatible status %s",
                      itr->ld_dev_path, op_status2str(itr->ld_op_status));
            continue;
        }

        if (!itr->ld_device_thread.ld_running) {
            pho_debug("Skipping ending or stopped device '%s'",
                      itr->ld_dev_path);
            continue;
        }

        /*
         * The intent is to write: exclude media that are administratively
         * locked, full, do not have the put operation flag and do not have the
         * requested tags
         */
        if (is_write && itr->ld_dss_media_info) {
            if (itr->ld_dss_media_info->rsc.adm_status !=
                    PHO_RSC_ADM_ST_UNLOCKED) {
                pho_debug("Media '%s' is not unlocked",
                          itr->ld_dss_media_info->rsc.id.name);
                continue;
            }

            if (itr->ld_dss_media_info->fs.status == PHO_FS_STATUS_FULL) {
                pho_debug("Media '%s' is full",
                          itr->ld_dss_media_info->rsc.id.name);
                continue;
            }

            if (!itr->ld_dss_media_info->flags.put) {
                pho_debug("Media '%s' has a false put operation flag",
                          itr->ld_dss_media_info->rsc.id.name);
                continue;
            }

            if (media_tags->n_tags > 0 &&
                !tags_in(&itr->ld_dss_media_info->tags, media_tags)) {
                pho_debug("Media '%s' does not match required tags",
                          itr->ld_dss_media_info->rsc.id.name);
                continue;
            }
        }

        /* check tape / drive compat */
        if (pmedia) {
            bool res;

            if (tape_drive_compat(pmedia, itr, &res)) {
                selected = NULL;
                break;
            }

            if (!res)
                continue;
        }

        rc = select_func(required_size, itr, &selected);
        if (prev != selected)
            selected_i = i;

        if (rc < 0) {
            pho_debug("Device selection function failed");
            selected = NULL;
            break;
        } else if (rc == 0) /* stop searching */
            break;
    }

    if (selected != NULL) {
        pho_debug("Picked dev number %d (%s)",
                  selected_i,
                  selected->ld_dev_path);
        selected->ld_ongoing_io = true;
    } else {
        pho_debug("Could not find a suitable %s device", op_status2str(op_st));
    }

    return selected;
}

/**
 * Get the first device with enough space.
 * @retval 0 to stop searching for a device
 * @retval 1 to check next device.
 */
static int select_first_fit(size_t required_size,
                            struct lrs_dev *dev_curr,
                            struct lrs_dev **dev_selected)
{
    ENTRY;

    if (dev_curr->ld_dss_media_info == NULL)
        return 1;

    if (dev_curr->ld_dss_media_info->stats.phys_spc_free >= required_size) {
        *dev_selected = dev_curr;
        return 0;
    }
    return 1;
}

/**
 *  Get the device with the lower space to match required_size.
 * @return 1 to check next devices, unless an exact match is found (return 0).
 */
static int select_best_fit(size_t required_size,
                           struct lrs_dev *dev_curr,
                           struct lrs_dev **dev_selected)
{
    ENTRY;

    if (dev_curr->ld_dss_media_info == NULL)
        return 1;

    /* does it fit? */
    if (dev_curr->ld_dss_media_info->stats.phys_spc_free < required_size)
        return 1;

    /* no previous fit, or better fit */
    if (*dev_selected == NULL ||
        (dev_curr->ld_dss_media_info->stats.phys_spc_free <
         (*dev_selected)->ld_dss_media_info->stats.phys_spc_free)) {
        *dev_selected = dev_curr;

        if (required_size == dev_curr->ld_dss_media_info->stats.phys_spc_free)
            /* exact match, stop searching */
            return 0;
    }
    return 1;
}

/**
 * Select any device without checking media or available size.
 * @return 0 on first device found, 1 else (to continue searching).
 */
static int select_any(size_t required_size,
                      struct lrs_dev *dev_curr,
                      struct lrs_dev **dev_selected)
{
    ENTRY;

    if (*dev_selected == NULL) {
        *dev_selected = dev_curr;
        /* found an item, stop searching */
        return 0;
    }
    return 1;
}

/* Get the device with the least space available on the loaded media.
 * If a tape is loaded, it just needs to be unloaded.
 * If the filesystem is mounted, umount is needed before unloading.
 * @return 1 (always check all devices).
 */
static int select_drive_to_free(size_t required_size,
                                struct lrs_dev *dev_curr,
                                struct lrs_dev **dev_selected)
{
    ENTRY;

    /* skip failed and busy drives */
    if (dev_curr->ld_op_status == PHO_DEV_OP_ST_FAILED ||
        dev_curr->ld_ongoing_io || dev_curr->ld_needs_sync ||
        !dev_curr->ld_device_thread.ld_running) {
        pho_debug("Skipping drive '%s' with status %s%s%s",
                  dev_curr->ld_dev_path,
                  op_status2str(dev_curr->ld_op_status),
                  dev_curr->ld_ongoing_io || dev_curr->ld_needs_sync ?
                  " (busy)" : "",
                  !dev_curr->ld_device_thread.ld_running ?
                        " (ending or stopped)" : "");
        return 1;
    }

    /* if this function is called, no drive should be empty */
    if (dev_curr->ld_op_status == PHO_DEV_OP_ST_EMPTY) {
        pho_warn("Unexpected drive status for '%s': '%s'",
                 dev_curr->ld_dev_path,
                 op_status2str(dev_curr->ld_op_status));
        return 1;
    }

    /* less space available on this device than the previous ones? */
    if (*dev_selected == NULL ||
        dev_curr->ld_dss_media_info->stats.phys_spc_free <
        (*dev_selected)->ld_dss_media_info->stats.phys_spc_free) {
        *dev_selected = dev_curr;
        return 1;
    }

    return 1;
}

/**
 * Mount the filesystem of a ready device
 *
 * Must be called with :
 * - dev->ld_ongoing_io set to true,
 * - dev->ld_op_status set to PHO_DEV_OP_ST_LOADED and a loaded
 *   dev->ld_dss_media_info
 * - a global DSS lock on dev
 * - a global DSS lock on dev->ld_dss_media_info
 *
 * On error, signal the error to the device thread.
 */
static int sched_mount(struct lrs_sched *sched, struct lrs_dev *dev)
{
    char                *mnt_root;
    struct fs_adapter    fsa;
    const char          *id;
    int                  rc;

    ENTRY;

    rc = get_fs_adapter(dev->ld_dss_media_info->fs.type, &fsa);
    if (rc)
        goto out;

    rc = ldm_fs_mounted(&fsa, dev->ld_dev_path, dev->ld_mnt_path,
                        sizeof(dev->ld_mnt_path));
    if (rc == 0) {
        dev->ld_op_status = PHO_DEV_OP_ST_MOUNTED;
        return 0;
    }

    /**
     * @TODO If library indicates a media is in the drive but the drive
     * doesn't, we need to query the drive to load the tape.
     */

    id = basename(dev->ld_dev_path);
    if (id == NULL)
        return -EINVAL;

    /* mount the device as PHO_MNT_PREFIX<id> */
    mnt_root = mount_point(id);
    if (!mnt_root)
        LOG_GOTO(out, rc = -ENOMEM, "Unable to get mount point of %s", id);

    pho_info("mount: device '%s' as '%s'", dev->ld_dev_path, mnt_root);

    rc = ldm_fs_mount(&fsa, dev->ld_dev_path, mnt_root,
                      dev->ld_dss_media_info->fs.label);
    if (rc)
        LOG_GOTO(out_free, rc, "Failed to mount device '%s'",
                 dev->ld_dev_path);

    /* update device state and set mount point */
    dev->ld_op_status = PHO_DEV_OP_ST_MOUNTED;
    strncpy(dev->ld_mnt_path,  mnt_root, sizeof(dev->ld_mnt_path));
    dev->ld_mnt_path[sizeof(dev->ld_mnt_path) - 1] = '\0';

out_free:
    free(mnt_root);
out:
    if (rc)
        dev_thread_signal_stop_on_error(dev, rc);

    return rc;
}

/**
 * Load a media into a drive.
 *
 * Must be called while owning a global DSS lock on dev and on media and with
 * the ld_ongoing_io flag set to true on dev.
 *
 * On error, unlock the media into DSS and signal the error to the device thread
 * if the error involves the device.
 *
 * @return 0 on success, -error number on error. -EBUSY is returned when a
 * drive to drive media movement was prevented by the library or if the device
 * is empty.
 */
static int sched_load_media(struct lrs_sched *sched, struct lrs_dev *dev,
                            struct media_info *media)
{
    int                  failure_on_dev = 0;
    struct lib_item_addr media_addr;
    struct lib_adapter   lib;
    int                  rc;
    int                  rc2;

    ENTRY;

    if (dev->ld_op_status != PHO_DEV_OP_ST_EMPTY)
        LOG_GOTO(out, rc = -EAGAIN, "%s: unexpected drive status: status='%s'",
                 dev->ld_dev_path, op_status2str(dev->ld_op_status));

    if (dev->ld_dss_media_info != NULL)
        LOG_GOTO(out, rc = -EAGAIN,
                 "No media expected in device '%s' (found '%s')",
                 dev->ld_dev_path, dev->ld_dss_media_info->rsc.id.name);

    pho_verb("Loading '%s' into '%s'", media->rsc.id.name, dev->ld_dev_path);

    /* get handle to the library depending on device type */
    rc = wrap_lib_open(dev->ld_dss_dev_info->rsc.id.family, &lib);
    if (rc)
        LOG_GOTO(out, failure_on_dev = rc, "Failed to open device lib");

    /* lookup the requested media */
    rc = ldm_lib_media_lookup(&lib, media->rsc.id.name, &media_addr);
    if (rc)
        LOG_GOTO(out_close, rc, "Media lookup failed");

    rc = ldm_lib_media_move(&lib, &media_addr, &dev->ld_lib_dev_info.ldi_addr);
    /* A movement from drive to drive can be prohibited by some libraries.
     * If a failure is encountered in such a situation, it probably means that
     * the state of the library has changed between the moment it has been
     * scanned and the moment the media and drive have been selected. The
     * easiest solution is therefore to return EBUSY to signal this situation to
     * the caller.
     */
    if (rc == -EINVAL
            && media_addr.lia_type == MED_LOC_DRIVE
            && dev->ld_lib_dev_info.ldi_addr.lia_type == MED_LOC_DRIVE) {
        pho_debug("Failed to move a media from one drive to another, trying "
                  "again later");
        /* @TODO: acquire source drive on the fly? */
        GOTO(out_close, rc = -EBUSY);
    } else if (rc != 0) {
        /* Set operationnal failure state on this drive. It is incomplete since
         * the error can originate from a defect tape too...
         *  - consider marking both as failed.
         *  - consider maintaining lists of errors to diagnose and decide who to
         *    exclude from the cool game.
         */
        LOG_GOTO(out_close, failure_on_dev = rc, "Media move failed");
    }

    /* update device status */
    dev->ld_op_status = PHO_DEV_OP_ST_LOADED;
    /* associate media to this device */
    dev->ld_dss_media_info = media;
    rc = 0;

out_close:
    rc2 = ldm_lib_close(&lib);
    if (rc2) {
        pho_error(rc2, "Unable to close lib");
        rc = rc ? : rc2;
    }

out:
    if (rc) {
        sched_medium_release(sched, media);
        if (failure_on_dev) {
            dev_thread_signal_stop_on_error(dev, failure_on_dev);
        } else {
            dev->ld_ongoing_io = false;
        }
    }

    return rc;
}

/** return the device policy function depending on configuration */
static device_select_func_t get_dev_policy(void)
{
    const char *policy_str;

    ENTRY;

    policy_str = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, policy);
    if (policy_str == NULL)
        return NULL;

    if (!strcmp(policy_str, "best_fit"))
        return select_best_fit;

    if (!strcmp(policy_str, "first_fit"))
        return select_first_fit;

    pho_error(EINVAL, "Invalid LRS policy name '%s' "
              "(expected: 'best_fit' or 'first_fit')", policy_str);

    return NULL;
}

/**
 * Return true if at least one compatible drive is found.
 *
 * The found compatible drive should be not failed, not locked by
 * administrator and not locked for the current operation.
 *
 * @param(in) pmedia          Media that should be used by the drive to check
 *                            compatibility (ignored if NULL, any not failed and
 *                            not administrator locked drive will fit.
 * @param(in) selected_devs   Devices already selected for this operation.
 * @param(in) n_selected_devs Number of devices already selected.
 * @return                    True if one compatible drive is found, else false.
 */
static bool compatible_drive_exists(struct lrs_sched *sched,
                                    struct media_info *pmedia,
                                    struct lrs_dev *selected_devs,
                                    const int n_selected_devs)
{
    int i, j;

    for (i = 0; i < sched->devices.ldh_devices->len; i++) {
        struct lrs_dev *dev = lrs_dev_hdl_get(&sched->devices, i);
        bool is_already_selected = false;

        if (dev->ld_op_status == PHO_DEV_OP_ST_FAILED ||
            !dev->ld_device_thread.ld_running)
            continue;

        /* check the device is not already selected */
        for (j = 0; j < n_selected_devs; ++j)
            if (!strcmp(dev->ld_dss_dev_info->rsc.id.name,
                        selected_devs[i].ld_dss_dev_info->rsc.id.name)) {
                is_already_selected = true;
                break;
            }
        if (is_already_selected)
            continue;

        if (pmedia) {
            bool is_compat;

            if (tape_drive_compat(pmedia, dev, &is_compat))
                continue;

            if (is_compat)
                return true;
        }
    }

    return false;
}

static int sched_empty_dev(struct lrs_sched *sched, struct lrs_dev *dev)
{
    int rc;

    if (dev->ld_op_status == PHO_DEV_OP_ST_MOUNTED) {
        rc = sched_umount(sched, dev);
        if (rc)
            return rc;
    }

    /**
     * We follow up on unload.
     * (a successful umount let the ld_op_status to LOADED)
     */
    if (dev->ld_op_status == PHO_DEV_OP_ST_LOADED)
        return sched_unload_medium(sched, dev);

    return 0;
}

/**
 * Free one of the devices to allow mounting a new media.
 * On success, the returned device is locked.
 * @param(out) lrs_dev       Pointer to an empty drive.
 * @param(in)  pmedia          Media that should be used by the drive to check
 *                             compatibility (ignored if NULL)
 * @param(in)  selected_devs   Devices already selected for this operation.
 * @param(in)  n_selected_devs Number of devices already selected.
 */
static int sched_free_one_device(struct lrs_sched *sched,
                                 struct lrs_dev **lrs_dev,
                                 struct media_info *pmedia,
                                 struct lrs_dev *selected_devs,
                                 const int n_selected_devs)
{
    struct lrs_dev *tmp_dev;

    ENTRY;

    while (1) {

        /* get a drive to free (PHO_DEV_OP_ST_UNSPEC for any state) */
        tmp_dev = dev_picker(sched, PHO_DEV_OP_ST_UNSPEC, select_drive_to_free,
                             0, &NO_TAGS, pmedia, false);
        if (tmp_dev == NULL) {
            if (compatible_drive_exists(sched, pmedia, selected_devs,
                                        n_selected_devs)) {
                pho_debug("No suitable device to free");
                return -EAGAIN;
            } else {
                LOG_RETURN(-ENODEV, "No compatible device exists not failed "
                                    "and not locked by admin");
            }
        }

        if (sched_empty_dev(sched, tmp_dev))
            continue;

        /* success: we've got an empty device */
        *lrs_dev = tmp_dev;
        return 0;
    }
}

/**
 * Get an additionnal prepared device to perform a write operation.
 * @param[in]     size          Size of the extent to be written.
 * @param[in]     tags          Tags used to filter candidate media, the
 *                              selected media must have all the specified tags.
 * @param[in/out] devs          Array of selected devices to write with.
 * @param[in]     new_dev_index Index of the new device to find. Devices from
 *                              0 to i-1 must be already allocated (with loaded
 *                              and mounted media)
 */
static int sched_get_write_res(struct lrs_sched *sched, size_t size,
                               const struct tags *tags, struct lrs_dev **devs,
                               size_t new_dev_index)
{
    struct lrs_dev **new_dev = &devs[new_dev_index];
    device_select_func_t dev_select_policy;
    struct media_info *pmedia;
    int rc;

    ENTRY;

    /*
     * @FIXME: externalize this to sched_responses_get to load the device state
     * only once per sched_responses_get call.
     */
    rc = sched_load_dev_state(sched);
    if (rc != 0)
        return rc;

    dev_select_policy = get_dev_policy();
    if (!dev_select_policy)
        return -EINVAL;

    pmedia = NULL;

    /* 1a) is there a mounted filesystem with enough room? */
    *new_dev = dev_picker(sched, PHO_DEV_OP_ST_MOUNTED, dev_select_policy,
                          size, tags, NULL, true);
    if (*new_dev != NULL)
        return 0;

    /* 1b) is there a loaded media with enough room? */
    *new_dev = dev_picker(sched, PHO_DEV_OP_ST_LOADED, dev_select_policy, size,
                          tags, NULL, true);
    if (*new_dev != NULL) {
        /* mount the filesystem and return */
        rc = sched_mount(sched, *new_dev);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to mount already loaded device '%s' from "
                       "writing", (*new_dev)->ld_dev_path);

        return 0;
    }

    /* V1: release a drive and load a tape with enough room.
     * later versions:
     * 2a) is there an idle drive, to eject the loaded tape?
     * 2b) is there an operation that will end soon?
     */
    /* 2) For the next steps, we need a media to write on.
     * It will be loaded into a free drive.
     * Note: sched_select_media locks the media.
     */

    pho_verb("Not enough space on loaded media: selecting another one");
    rc = sched_select_media(sched, &pmedia, size, sched->family, tags,
                            devs, new_dev_index);
    if (rc)
        return rc;

    /**
     * Check if the media is already in a drive.
     *
     * We already look for loaded media with full available size.
     *
     * sched_select_media could find a "split" medium which is already loaded
     * if there is no medium with a enough available size.
     */
    *new_dev = search_loaded_media(sched, pmedia->rsc.id.name);
    if (*new_dev != NULL) {
        media_info_free(pmedia);
        (*new_dev)->ld_ongoing_io = true;
        if ((*new_dev)->ld_op_status != PHO_DEV_OP_ST_MOUNTED)
            return sched_mount(sched, *new_dev);

        return 0;
    }

    /* 3) is there a free drive? */
    *new_dev = dev_picker(sched, PHO_DEV_OP_ST_EMPTY, select_any, 0, &NO_TAGS,
                          pmedia, true);
    if (*new_dev == NULL) {
        pho_verb("No free drive: need to unload one");
        rc = sched_free_one_device(sched, new_dev, pmedia, *devs,
                                   new_dev_index);
        if (rc) {
            sched_medium_release(sched, pmedia);
            media_info_free(pmedia);
            /** TODO: maybe we can try to select an other type of media */
            return rc;
        }
    }

    /* 4) load the selected media into the selected drive */
    rc = sched_load_media(sched, *new_dev, pmedia);
    if (rc) {
        media_info_free(pmedia);
        return rc;
    }

    /* 5) mount the filesystem */
    return sched_mount(sched, *new_dev);
}

static int sched_media_lock_and_load(struct lrs_sched *sched,
                                     struct media_info *media,
                                     struct lrs_dev **dev)
{
    int rc;

    if (media->lock.hostname) {
        rc = check_renew_lock(sched, DSS_MEDIA, media, &media->lock);
        if (rc)
            LOG_RETURN(rc,
                       "Unable to renew an existing lock before loading media");
    } else {
        rc = take_and_update_lock(&sched->dss, DSS_MEDIA, media, &media->lock);
        if (rc != 0)
            LOG_RETURN(rc = -EAGAIN,
                       "Unable to take lock before loading media");
    }

    /* Is there a free drive? */
    *dev = dev_picker(sched, PHO_DEV_OP_ST_EMPTY, select_any, 0, &NO_TAGS,
                      media, false);
    if (*dev == NULL) {
        pho_verb("No free drive: need to unload one");
        rc = sched_free_one_device(sched, dev, media, NULL, 0);
        if (rc != 0) {
            sched_medium_release(sched, media);
            pho_verb("No device available");
            return rc;
        }
    }

    /* load the media in it */
    rc = sched_load_media(sched, *dev, media);
    if (rc)
        LOG_RETURN(rc, "Unable to load medium '%s' into device '%s' when "
                       "preparing media",
                   media->rsc.id.name, (*dev)->ld_dev_path);

    return 0;
}

static int
check_read_medium_permission_and_status(const struct media_info *medium)
{
    if (!medium->flags.get)
        LOG_RETURN(-EPERM, "'%s' get flag is false",
                   medium->rsc.id.name);
    if (medium->fs.status == PHO_FS_STATUS_BLANK)
        LOG_RETURN(-EINVAL, "Cannot do I/O on unformatted medium '%s'",
                   medium->rsc.id.name);
    if (medium->rsc.adm_status != PHO_RSC_ADM_ST_UNLOCKED)
        LOG_RETURN(-EPERM, "Cannot do I/O on an admin locked medium '%s'",
                   medium->rsc.id.name);

    return 0;
}

static int sched_media_prepare_for_read(struct lrs_sched *sched,
                                        const struct pho_id *id,
                                        struct lrs_dev **pdev,
                                        struct media_info **pmedia)
{
    struct media_info *med = NULL;
    struct lrs_dev *dev;
    int rc;

    ENTRY;

    *pdev = NULL;
    *pmedia = NULL;

    rc = sched_fill_media_info(sched, &med, id);
    if (rc == -EALREADY) {
        pho_debug("Media '%s' is locked, returning EAGAIN", id->name);
        GOTO(out, rc = -EAGAIN);
    } else if (rc) {
        return rc;
    }

    rc = check_read_medium_permission_and_status(med);
    if (rc)
        LOG_GOTO(out, rc,
                 "Cannot read on medium '%s' due to permission or status "
                 "mismatch", id->name);

    /* check if the media is already in a drive */
    dev = search_loaded_media(sched, id->name);
    if (dev != NULL) {
        if (dev->ld_ongoing_io)
            LOG_GOTO(out, rc = -EAGAIN,
                     "Media '%s' is loaded in an already used drive '%s'",
                     id->name, dev->ld_dev_path);

        dev->ld_ongoing_io = true;
        /* Media is in dev, update dev->ld_dss_media_info with fresh media info
         */
        media_info_free(dev->ld_dss_media_info);
        dev->ld_dss_media_info = med;
    } else {
        rc = sched_media_lock_and_load(sched, med, &dev);
        if (rc)
            goto out;
    }
    /* at this point, med is transfered to dev->ld_dss_media_info */
    med = NULL;

    /* Mount only if not already mounted */
    if (dev->ld_op_status != PHO_DEV_OP_ST_MOUNTED)
        rc = sched_mount(sched, dev);

out:
    if (rc) {
        media_info_free(med);
        *pmedia = NULL;
        *pdev = NULL;
    } else {
        *pmedia = dev->ld_dss_media_info;
        *pdev = dev;
    }

    return rc;
}

static bool sched_mount_is_writable(const char *fs_root, enum fs_type fs_type)
{
    struct ldm_fs_space  fs_info = {0};
    struct fs_adapter    fsa;
    int                  rc;

    rc = get_fs_adapter(fs_type, &fsa);
    if (rc)
        LOG_RETURN(rc, "No FS adapter found for '%s' (type %s)",
                   fs_root, fs_type2str(fs_type));

    rc = ldm_fs_df(&fsa, fs_root, &fs_info);
    if (rc)
        LOG_RETURN(rc, "Cannot retrieve media usage information");

    return !(fs_info.spc_flags & PHO_FS_READONLY);
}

/**
 * Query to write a given amount of data by acquiring a new device with medium
 *
 * @param(in)     sched         Initialized LRS.
 * @param(in)     write_size    Size that will be written on the medium.
 * @param(in)     tags          Tags used to select a medium to write on, the
 *                              selected medium must have the specified tags.
 * @param(in/out) devs          Array of devices with the reserved medium
 *                              mounted and loaded in it (no need to free it).
 * @param(in)     new_dev_index Index in dev of the new device to alloc (devices
 *                              from 0 to i-1 must be already allocated : medium
 *                              mounted and loaded)
 *
 * @return 0 on success, -1 * posix error code on failure
 */
static int sched_write_prepare(struct lrs_sched *sched, size_t write_size,
                               const struct tags *tags,
                               struct lrs_dev **devs, int new_dev_index)
{
    struct media_info  *media = NULL;
    struct lrs_dev   *new_dev;
    int                 rc;

    ENTRY;

retry:
    rc = sched_get_write_res(sched, write_size, tags, devs, new_dev_index);
    if (rc != 0)
        return rc;

    new_dev = devs[new_dev_index];
    media = new_dev->ld_dss_media_info;

    /* LTFS can cunningly mount almost-full tapes as read-only, and so would
     * damaged disks. Mark the media as full, let it be mounted and try to find
     * a new one.
     */
    if (!sched_mount_is_writable(new_dev->ld_mnt_path,
                                 media->fs.type)) {
        pho_warn("Media '%s' OK but mounted R/O, marking full and retrying...",
                 media->rsc.id.name);

        media->fs.status = PHO_FS_STATUS_FULL;
        new_dev->ld_ongoing_io = false;

        rc = dss_media_set(&sched->dss, media, 1, DSS_SET_UPDATE, FS_STATUS);
        if (rc)
            LOG_RETURN(rc, "Unable to update DSS media '%s' status to FULL",
                       media->rsc.id.name);

        new_dev = NULL;
        media = NULL;
        goto retry;
    }

    pho_debug("write: media '%s' using device '%s' (free space: %zu bytes)",
             media->rsc.id.name, new_dev->ld_dev_path,
             new_dev->ld_dss_media_info->stats.phys_spc_free);

    return 0;
}

/**
 * Query to read from a given set of medium.
 *
 * @param(in)     sched   Initialized LRS.
 * @param(in)     id      The id of the medium to load
 * @param(out)    dev     Device with the required medium mounted and loaded in
 *                        it (no need to free it).
 *
 * @return 0 on success, -1 * posix error code on failure
 */
static int sched_read_prepare(struct lrs_sched *sched,
                              const struct pho_id *id, struct lrs_dev **dev)
{
    struct media_info   *media_info = NULL;
    int                  rc;

    ENTRY;

    rc = sched_load_dev_state(sched);
    if (rc != 0)
        return rc;

    /* Fill in information about media and mount it if needed */
    rc = sched_media_prepare_for_read(sched, id, dev, &media_info);
    if (rc)
        return rc;

    if ((*dev)->ld_dss_media_info == NULL)
        LOG_GOTO(out, rc = -EINVAL, "Invalid device state, expected media '%s'",
                 id->name);

    pho_debug("read: media '%s' using device '%s'",
             media_info->rsc.id.name, (*dev)->ld_dev_path);

out:
    /* Don't free media_info since it is still referenced inside dev */
    return rc;
}

/******************************************************************************/
/* Request/response manipulation **********************************************/
/******************************************************************************/

static int sched_device_add(struct lrs_sched *sched, enum rsc_family family,
                            const char *name)
{
    struct lrs_dev *device = NULL;
    struct lib_adapter lib;
    int rc = 0;

    rc = lrs_dev_hdl_add(sched, &sched->devices, name);
    if (rc)
        return rc;

    device = lrs_dev_hdl_get(&sched->devices,
                             sched->devices.ldh_devices->len - 1);

    /* get a handle to the library to query it */
    rc = wrap_lib_open(family, &lib);
    if (rc)
        goto dev_del;

    MUTEX_LOCK(&device->ld_mutex);
    rc = sched_fill_dev_info(sched, &lib, device);
    MUTEX_UNLOCK(&device->ld_mutex);
    if (rc)
        goto err_lib;

    return 0;

err_lib:
    ldm_lib_close(&lib);
dev_del:
    lrs_dev_hdl_del(&sched->devices, sched->devices.ldh_devices->len - 1, rc);

    return rc;
}

/**
 * Remove the locked device from the local device array.
 * It will be inserted back once the device status is changed to 'unlocked'.
 */
static int sched_device_lock(struct lrs_sched *sched, const char *name)
{
    struct lrs_dev *dev;
    int i;

    for (i = 0; i < sched->devices.ldh_devices->len; ++i) {
        dev = lrs_dev_hdl_get(&sched->devices, i);

        if (!strcmp(name, dev->ld_dss_dev_info->rsc.id.name)) {
            lrs_dev_hdl_del(&sched->devices, i, 0);
            pho_verb("Removed locked device '%s' from the local database",
                     name);
            return 0;
        }
    }

    pho_verb("Cannot find local device info for '%s', not critical, "
             "will continue", name);

    return 0;
}

/**
 * Update local admin status of device to 'unlocked',
 * or fetch it from the database if unknown
 */
static int sched_device_unlock(struct lrs_sched *sched, const char *name)
{
    struct lrs_dev *dev;
    int i;

    for (i = 0; i < sched->devices.ldh_devices->len; ++i) {
        dev = lrs_dev_hdl_get(&sched->devices, i);

        if (!strcmp(name, dev->ld_dss_dev_info->rsc.id.name)) {
            pho_verb("Updating device '%s' state to unlocked", name);
            dev->ld_dss_dev_info->rsc.adm_status = PHO_RSC_ADM_ST_UNLOCKED;
            return 0;
        }
    }

    pho_verb("Cannot find local device info for '%s', will fetch it "
             "from the database", name);

    return sched_device_add(sched, sched->family, name);
}

/** remove written_size from phys_spc_free in media_info and DSS */
static int update_phys_spc_free(struct dss_handle *dss,
                                struct media_info *dss_media_info,
                                size_t written_size)
{
    if (written_size > 0) {
        dss_media_info->stats.phys_spc_free -= written_size;
        return dss_media_set(dss, dss_media_info, 1, DSS_SET_UPDATE,
                             PHYS_SPC_FREE);
    }

    return 0;
}

int sched_release_enqueue(struct lrs_sched *sched, struct req_container *reqc)
{
    int rc = 0;
    size_t i;
    int rc2;

    /* for each nosync_medium */
    for (i = 0; i < reqc->params.release.n_nosync_media; i++) {
        struct lrs_dev *dev;

        /* find the corresponding device */
        dev = search_loaded_media(sched,
            reqc->params.release.nosync_media[i].medium.name);
        if (dev && !dev->ld_device_thread.ld_running) {
            pho_error(-ENODEV, "device '%s' is not running but contains nosync "
                      "release medium '%s'", dev->ld_dss_dev_info->rsc.id.name,
                      reqc->params.release.nosync_media[i].medium.name);
            dev = NULL;
        }

        if (dev == NULL) {
            pho_error(-ENODEV, "Unable to find loaded device of the nosync "
                      "medium '%s'",
                      reqc->params.release.nosync_media[i].medium.name);
            continue;
        }

        /* update media phys_spc_free stats in advance, before next sync */
        /**
         * TODO: we save media status into DSS but we can also use local cached
         * values.
         *
         * Many modifications are needed:
         * - remove sched_load_dev_state from format/read/write
         * - take into account current cached value for loaded media into
         *   sched_select_media
         */
        MUTEX_LOCK(&dev->ld_mutex);
        rc2 = update_phys_spc_free(&sched->dss, dev->ld_dss_media_info,
            reqc->params.release.nosync_media[i].written_size);
        MUTEX_UNLOCK(&dev->ld_mutex);
        if (rc2) {
            pho_error(rc2, "Unable to update phys_spc_free");
            /*
             * TODO: returning a fatal error seems to much here,
             * only the medium and the device should be failed
             */
            rc = rc ? : rc2;
        }

        /* Acknowledgement of the request */
        dev->ld_ongoing_io = false;
    }

    if (!reqc->params.release.n_tosync_media) {
        sched_req_free(reqc);
        return rc;
    }

    /* for each tosync_medium */
    for (i = 0; i < reqc->params.release.n_tosync_media; i++) {
        struct lrs_dev *dev;
        int result_rc;

        /* find the corresponding device */
        dev = search_loaded_media(sched,
            reqc->params.release.tosync_media[i].medium.name);

        if (dev && !dev->ld_device_thread.ld_running) {
            pho_error(-ENODEV, "device '%s' is not running but contains tosync "
                      "release medium '%s'", dev->ld_dss_dev_info->rsc.id.name,
                      reqc->params.release.nosync_media[i].medium.name);
            dev = NULL;
        }

        if (dev != NULL) {
            /* update media phys_spc_free stats in advance, before next sync */
            /**
             * TODO: better to use a local cached value than always updating DSS
             *
             * Many modifications are needed:
             * - remove sched_load_dev_state from format/read/write
             * - take into account current cached value for loaded media into
             *   sched_select_media
             */
            MUTEX_LOCK(&dev->ld_mutex);
            rc2 = update_phys_spc_free(&sched->dss, dev->ld_dss_media_info,
                reqc->params.release.tosync_media[i].written_size);
            MUTEX_UNLOCK(&dev->ld_mutex);
            if (rc2) {
                pho_error(rc2, "Unable to update phys_spc_free");
                /*
                 * TODO: returning a fatal error seems to much here,
                 * only the medium and the device should be failed
                 */
                rc = rc ? : rc2;
            }

            /* Acknowledgement of the request */
            dev->ld_ongoing_io = false;

            /* Queue sync request */
            result_rc = push_new_sync_to_device(dev, reqc, i);

            if (result_rc)
                rc = rc ? : result_rc;
        } else {
            result_rc = -ENODEV;
        }

        MUTEX_LOCK(&reqc->mutex);

        reqc->params.release.rc = result_rc;
        if (!reqc->params.release.rc)
            goto unlock;

        /* manage error */
        reqc->params.release.tosync_media[i].status = SYNC_ERROR;
        rc2 = queue_error_response(sched->response_queue,
                                   reqc->params.release.rc,
                                   reqc);
        rc = rc ? : rc2;

        if (i != 0) {
            size_t j;

            for (j = i + 1; j < reqc->params.release.n_tosync_media; j++)
                reqc->params.release.tosync_media[j].status = SYNC_CANCEL;
        }

unlock:
        MUTEX_UNLOCK(&reqc->mutex);

        if (reqc->params.release.rc) {
            if (i == 0) {
                /* never queued: we can free it */
                sched_req_free(reqc);
            }

            break;
        }
    }

    return rc;
}

/*
 * @FIXME: this assumes one media is reserved for only one request. In the
 * future, we may want to give a media allocation to multiple requests, we will
 * therefore need to be more careful not to call sched_device_release or
 * sched_medium_release too early, or count nested locks.
 */
/**
 * Handle a write allocation request by finding an appropriate medias to write
 * to and mounting them.
 *
 * The request succeeds totally or all the performed allocations are rolled
 * back.
 */
static int sched_handle_write_alloc(struct lrs_sched *sched, pho_req_t *req,
                                    struct resp_container *respc)
{
    pho_req_write_t *wreq = req->walloc;
    pho_resp_t *resp = respc->resp;
    struct lrs_dev **devs = NULL;
    size_t i = 0;
    int rc = 0;

    pho_debug("write: allocation request (%ld medias)", wreq->n_media);

    rc = pho_srl_response_write_alloc(resp, wreq->n_media);
    if (rc)
        return rc;

    devs = calloc(wreq->n_media, sizeof(*devs));
    if (devs == NULL)
        return -errno;

    respc->devices = malloc(wreq->n_media * sizeof(*respc->devices));
    if (!respc->devices)
        GOTO(out, rc = -errno);

    respc->devices_len = wreq->n_media;
    resp->req_id = req->id;

    /*
     * @TODO: if media locking becomes ref counted, ensure all selected medias
     * are different
     */
    for (i = 0; i < wreq->n_media; i++) {
        struct tags t;

        pho_resp_write_elt_t *wresp = resp->walloc->media[i];

        pho_debug("Write allocation request media %ld: need %ld bytes",
                  i, wreq->media[i]->size);

        t.n_tags = wreq->media[i]->n_tags;
        t.tags = wreq->media[i]->tags;

        rc = sched_write_prepare(sched, wreq->media[i]->size, &t, devs, i);
        if (rc)
            goto out;

        /* build response */
        wresp->avail_size = devs[i]->ld_dss_media_info->stats.phys_spc_free;
        wresp->med_id->family = devs[i]->ld_dss_media_info->rsc.id.family;
        wresp->med_id->name = strdup(devs[i]->ld_dss_media_info->rsc.id.name);
        wresp->root_path = strdup(devs[i]->ld_mnt_path);
        wresp->fs_type = devs[i]->ld_dss_media_info->fs.type;
        wresp->addr_type = devs[i]->ld_dss_media_info->addr_type;
        respc->devices[i] = devs[i];

        if (wresp->root_path == NULL) {
            /*
             * Increment i so that the currently selected media is released
             * as well in cleanup
             */
            i++;
            GOTO(out, rc = -ENOMEM);
        }
    }

out:
    free(devs);

    if (rc) {
        size_t n_media_acquired = i;

        free(respc->devices);
        respc->devices_len = 0;
        /* Rollback device and media acquisition */
        for (i = 0; i < n_media_acquired; i++) {
            struct lrs_dev *dev;
            pho_resp_write_elt_t *wresp = resp->walloc->media[i];

            dev = search_loaded_media(sched, wresp->med_id->name);
            assert(dev != NULL);
            dev->ld_ongoing_io = false;
        }

        pho_srl_response_free(resp, false);
        if (rc != -EAGAIN) {
            int rc2 = pho_srl_response_error_alloc(resp);

            if (rc2)
                return rc2;

            resp->req_id = req->id;
            resp->error->rc = rc;
            resp->error->req_kind = PHO_REQUEST_KIND__RQ_WRITE;

            /* Request processing error, not an LRS error */
            rc = 0;
        }
    }

    return rc;
}

/**
 * Handle a read allocation request by finding the specified medias and mounting
 * them.
 *
 * The request succeeds totally or all the performed allocations are rolled
 * back.
 */
static int sched_handle_read_alloc(struct lrs_sched *sched, pho_req_t *req,
                                   struct resp_container *respc)
{
    pho_req_read_t *rreq = req->ralloc;
    pho_resp_t *resp = respc->resp;
    struct lrs_dev *dev = NULL;
    size_t n_selected = 0;
    int rc = 0;
    size_t i;

    respc->devices = malloc(rreq->n_required * sizeof(*respc->devices));
    if (!respc->devices)
        return -errno;

    rc = pho_srl_response_read_alloc(resp, rreq->n_required);
    if (rc)
        goto free_devices;

    pho_debug("read: allocation request (%ld medias)", rreq->n_med_ids);

    /*
     * FIXME: this is a very basic selection algorithm that does not try to
     * select the most available media first.
     */
    for (i = 0; i < rreq->n_med_ids; i++) {
        pho_resp_read_elt_t *rresp = resp->ralloc->media[n_selected];
        struct pho_id m;

        m.family = (enum rsc_family)rreq->med_ids[i]->family;
        pho_id_name_set(&m, rreq->med_ids[i]->name);

        rc = sched_read_prepare(sched, &m, &dev);
        if (rc)
            continue;

        rresp->fs_type = dev->ld_dss_media_info->fs.type;
        rresp->addr_type = dev->ld_dss_media_info->addr_type;
        rresp->root_path = strdup(dev->ld_mnt_path);
        rresp->med_id->family = rreq->med_ids[i]->family;
        rresp->med_id->name = strdup(rreq->med_ids[i]->name);
        respc->devices[n_selected] = dev;

        n_selected++;

        if (n_selected == rreq->n_required)
            break;
    }

    if (rc) {
        /* rollback */
        for (i = 0; i < n_selected; i++) {
            pho_resp_read_elt_t *rresp = resp->ralloc->media[i];

            dev = search_loaded_media(sched, rresp->med_id->name);
            assert(dev != NULL);
            dev->ld_ongoing_io = false;
        }

        pho_srl_response_free(resp, false);
        if (rc != -EAGAIN) {
            int rc2 = pho_srl_response_error_alloc(resp);

            if (rc2)
                GOTO(free_devices, rc = rc2);

            resp->req_id = req->id;
            resp->error->rc = rc;
            resp->error->req_kind = PHO_REQUEST_KIND__RQ_READ;

            /* Request processing error, not an LRS error */
            rc = 0;
        }
    }

free_devices:
    if (rc) {
        free(respc->devices);
        respc->devices_len = 0;
    }
    return rc;
}

/**
 * Handle a format request
 *
 * reqc is freed, except when -EAGAIN is returned.
 */
static int sched_handle_format(struct lrs_sched *sched,
                               struct req_container *reqc)
{
    pho_req_format_t *freq = reqc->req->format;
    struct lrs_dev *device = NULL;
    struct pho_id m;
    int rc = 0;

    m.family = (enum rsc_family)freq->med_id->family;
    pho_id_name_set(&m, freq->med_id->name);
    rc = sched_fill_media_info(sched, &reqc->params.format.medium_to_format,
                               &m);
    if (rc) {
        if (rc == -EALREADY)
            LOG_GOTO(err_out, rc = -EBUSY,
                     "medium '%s' to format is already locked", m.name);
        else
            goto err_out;
    }

    if (reqc->params.format.medium_to_format->fs.status != PHO_FS_STATUS_BLANK)
        LOG_GOTO(err_out, rc = -EINVAL,
                 "Medium '%s' has a fs.status '%s', expected "
                 "PHO_FS_STATUS_BLANK to be formatted", m.name,
                 fs_status2str(
                    reqc->params.format.medium_to_format->fs.status));

    rc = get_fs_adapter((enum fs_type)freq->fs, &reqc->params.format.fsa);
    if (rc)
        LOG_GOTO(err_out, rc, "Invalid fs_type (%d)", freq->fs);

    if (!format_medium_add(&sched->ongoing_format,
                           reqc->params.format.medium_to_format))
        LOG_GOTO(err_out, rc = -EINVAL,
                 "trying to format the medium '%s' while it is already being "
                 "formatted", m.name);

    device = search_loaded_media(sched, m.name);
    if (device) {
        if (device->ld_ongoing_io || device->ld_needs_sync)
            LOG_GOTO(remove_format_err_out, rc = -EINVAL,
                     "medium %s is already loaded into a busy device %s, "
                     "unexpected state, will abort request",
                     m.name, device->ld_dss_dev_info->rsc.id.name);

        device->ld_ongoing_io = true;
    } else {
        /* medium to format isn't already loaded into any drive, need lock */
        if (!reqc->params.format.medium_to_format->lock.hostname) {
            rc = take_and_update_lock(&sched->dss, DSS_MEDIA,
                    reqc->params.format.medium_to_format,
                    &reqc->params.format.medium_to_format->lock);
            if (rc == -EEXIST)
                LOG_GOTO(remove_format_err_out, rc = -EBUSY,
                         "Media '%s' is locked by an other LRS node", m.name);
            else if (rc)
                LOG_GOTO(remove_format_err_out, rc,
                         "Unable to lock the media '%s' to format it", m.name);
        }

        /*
         * TODO: refresh a global dev_picker function to avoid scrolling 3 times
         *       the devices list and to differentiate errors between all
         *       devices are busy (returning -EAGAIN) or no compatible device
         *       (sending a format error response)
         */
        device = dev_picker(sched, PHO_DEV_OP_ST_EMPTY, select_any, 0, &NO_TAGS,
                            reqc->params.format.medium_to_format, false);
        if (!device)
            device = dev_picker(sched, PHO_DEV_OP_ST_LOADED, select_any, 0,
                                &NO_TAGS, reqc->params.format.medium_to_format,
                                false);

        if (!device)
            device = dev_picker(sched, PHO_DEV_OP_ST_MOUNTED, select_any, 0,
                                &NO_TAGS, reqc->params.format.medium_to_format,
                                false);

        if (!device) {
            pho_verb("Unable to find an available device to format medium '%s'",
                     m.name);
            format_medium_remove(&sched->ongoing_format,
                                 reqc->params.format.medium_to_format);
            return -EAGAIN;
        }
    }

    device->ld_format_request = reqc;
    return 0;

remove_format_err_out:
    format_medium_remove(&sched->ongoing_format,
                         reqc->params.format.medium_to_format);

err_out:
    if (rc != -EAGAIN) {
        int rc2;

        pho_error(rc, "format: failed to schedule format for medium '%s'",
                  m.name);
        rc2 = queue_error_response(sched->response_queue, rc, reqc);
        sched_req_free(reqc);
        if (rc2)
            pho_error(rc2, "Error on sending format error response");

        /*
         * LRS global error only if we face a response error.
         * If there is no response error, rc2 is equal to zero and we set rc to
         * the no error zero value, else we track the response error rc2 into
         * rc.
         */
        rc = rc2;
    }

    return rc;
}

static int sched_handle_notify(struct lrs_sched *sched, pho_req_t *req,
                               pho_resp_t *resp)
{
    pho_req_notify_t *nreq = req->notify;
    int rc = 0;

    rc = pho_srl_response_notify_alloc(resp);
    if (rc)
        return rc;

    pho_info("notify: media %s\n", nreq->rsrc_id->name);

    switch (nreq->op) {
    case PHO_NTFY_OP_DEVICE_ADD:
        rc = sched_device_add(sched, (enum rsc_family)nreq->rsrc_id->family,
                              nreq->rsrc_id->name);
        break;
    case PHO_NTFY_OP_DEVICE_LOCK:
        rc = sched_device_lock(sched, nreq->rsrc_id->name);
        break;
    case PHO_NTFY_OP_DEVICE_UNLOCK:
        rc = sched_device_unlock(sched, nreq->rsrc_id->name);
        break;
    default:
        LOG_GOTO(err, rc = -EINVAL, "The requested operation is not "
                 "recognized");
    }

    if (rc)
        goto err;

    resp->req_id = req->id;
    resp->notify->rsrc_id->family = nreq->rsrc_id->family;
    resp->notify->rsrc_id->name = strdup(nreq->rsrc_id->name);

    return rc;

err:
    pho_srl_response_free(resp, false);

    if (rc != -EAGAIN) {
        int rc2 = pho_srl_response_error_alloc(resp);

        if (rc2)
            return rc2;

        resp->req_id = req->id;
        resp->error->rc = rc;
        resp->error->req_kind = PHO_REQUEST_KIND__RQ_NOTIFY;

        /* Request processing error, not an LRS error */
        rc = 0;
    }

    return rc;
}

int sched_responses_get(struct lrs_sched *sched, int *n_resp,
                        struct resp_container **respc)
{
    struct req_container *reqc;
    GArray *resp_array;
    int rc = 0;

    resp_array = g_array_sized_new(FALSE, FALSE, sizeof(struct resp_container),
                                   tsqueue_get_length(&sched->req_queue));
    if (resp_array == NULL)
        return -ENOMEM;
    g_array_set_clear_func(resp_array, sched_resp_free);

    /*
     * Very simple algorithm (FIXME): serve requests until the first EAGAIN is
     * encountered.
     */
    while ((reqc = tsqueue_pop(&sched->req_queue)) != NULL) {
        pho_req_t *req = reqc->req;

        if (!running) {
            rc = queue_error_response(sched->response_queue, -ESHUTDOWN, reqc);
            sched_req_free(reqc);
            if (rc)
                break;
        } else if (pho_request_is_format(req)) {
            pho_debug("lrs received format request (%p)", req);
            rc = sched_handle_format(sched, reqc);
        } else {
            struct resp_container *respc;

            g_array_set_size(resp_array, resp_array->len + 1);
            respc = &g_array_index(resp_array, struct resp_container,
                                   resp_array->len - 1);
            respc->socket_id = reqc->socket_id;
            respc->resp = malloc(sizeof(*respc->resp));
            if (!respc->resp) {
                sched_req_free(reqc);
                LOG_GOTO(out, rc = -ENOMEM, "lrs cannot allocate response");
            }

            if (pho_request_is_write(req)) {
                pho_debug("lrs received write request (%p)", req);
                rc = sched_handle_write_alloc(sched, req, respc);
            } else if (pho_request_is_read(req)) {
                pho_debug("lrs received read allocation request (%p)", req);
                rc = sched_handle_read_alloc(sched, req, respc);
            } else if (pho_request_is_notify(req)) {
                pho_debug("lrs received notify request (%p)", req);
                rc = sched_handle_notify(sched, req, respc->resp);
            } else {
                /* Unexpected req->kind, very probably a programming error */
                pho_error(rc = -EPROTO,
                          "lrs received an invalid request "
                          "(no walloc, ralloc or release field)");
            }

            if (rc != -EAGAIN)
                sched_req_free(reqc);
        }

        if (rc == 0)
            continue;

        if (rc != -EAGAIN)
            break;

        /* no response to -EAGAIN */
        if (!pho_request_is_format(reqc->req))
            g_array_remove_index(resp_array, resp_array->len - 1);

        if (running) {
            /* Requeue last request on -EAGAIN and running */
            tsqueue_push(&sched->req_queue, reqc);
            rc = 0;
            break;
        }

        /* create an -ESHUTDOWN error on -EAGAIN and !running */
        rc = queue_error_response(sched->response_queue, -ESHUTDOWN, reqc);
        sched_req_free(reqc);
        if (rc)
            break;
    }

out:
    /* Error return means a fatal error for this LRS (FIXME) */
    if (rc) {
        g_array_free(resp_array, TRUE);
    } else {
        *n_resp = resp_array->len;
        *respc = (struct resp_container *)g_array_free(resp_array, FALSE);
    }

    /*
     * Media that have not been re-acquired at this point could be "globally
     * unlocked" here rather than at the beginning of this function.
     */

    return rc;
}

static void _json_object_set_str(struct json_t *object,
                                 const char *key,
                                 const char *value)
{
    json_t *str;

    if (!value)
        return;

    str = json_string(value);
    if (!str)
        return;

    json_object_set(object, key, str);
    json_decref(str);
}

static void sched_fetch_device_status(struct lrs_dev *device,
                                      json_t *device_status)
{
    json_t *integer;

    _json_object_set_str(device_status, "name", device->ld_dss_dev_info->path);
    _json_object_set_str(device_status, "device", device->ld_dev_path);
    _json_object_set_str(device_status, "serial",
                         device->ld_sys_dev_state.lds_serial);

    integer = json_integer(device->ld_lib_dev_info.ldi_addr.lia_addr -
                           device->ld_lib_dev_info.ldi_first_addr);
    if (integer) {
        json_object_set(device_status, "address", integer);
        json_decref(integer);
    }

    if (device->ld_dss_media_info) {
        json_t *ongoing_io;

        _json_object_set_str(device_status, "mount_path", device->ld_mnt_path);
        _json_object_set_str(device_status, "media",
                             device->ld_dss_media_info->rsc.id.name);

        ongoing_io = json_boolean(device->ld_ongoing_io);
        if (ongoing_io) {
            json_object_set(device_status, "ongoing_io", ongoing_io);
            json_decref(ongoing_io);
        }
    }
}

int sched_handle_monitor(struct lrs_sched *sched, json_t *status)
{
    json_t *device_status;
    int rc = 0;
    int i;

    for (i = 0; i < sched->devices.ldh_devices->len; i++) {
        struct lrs_dev *device;

        device_status = json_object();
        if (!device_status)
            LOG_RETURN(-ENOMEM, "Failed to allocate device_status");

        device = lrs_dev_hdl_get(&sched->devices, i);

        sched_fetch_device_status(device, device_status);

        rc = json_array_append_new(status, device_status);
        if (rc == -1)
            LOG_GOTO(free_device_status,
                     rc = -ENOMEM,
                     "Failed to append device status to array");

        continue;

free_device_status:
        json_decref(device_status);
    }

    return rc;
}
