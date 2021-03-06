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
 * \brief  LRS Device Thread handling
 */
#ifndef _PHO_LRS_DEVICE_H
#define _PHO_LRS_DEVICE_H

#include <glib.h>
#include <pthread.h>
#include <stdbool.h>

#include "pho_dss.h"
#include "pho_ldm.h"
#include "pho_types.h"

struct lrs_sched;

/**
 * Structure handling thread devices used by the scheduler.
 */
struct lrs_dev_hdl {
    GPtrArray      *ldh_devices;   /**< List of active devices of
                                     *  type lrs_dev
                                     */
    struct timespec sync_time_ms;  /**< Time threshold for medium
                                     *  synchronization
                                     */
    unsigned int    sync_nb_req;   /**< Number of requests
                                     *  threshold for medium
                                     *  synchronization
                                     */
    unsigned long   sync_wsize_kb; /**< Written size threshold for
                                     *  medium synchronization
                                     */
};

/**
 * Internal state of the device thread.
 */
struct thread_info {
    pthread_t            ld_tid;            /**< ID of the thread handling
                                              *  this device.
                                              */
    pthread_mutex_t      ld_signal_mutex;   /**< Mutex to protect the signal
                                              * access.
                                              */
    pthread_cond_t       ld_signal;         /**< Used to signal the thread
                                              * when new work is available.
                                              */
    bool                 ld_running;        /**< true as long as the thread
                                              * should run.
                                              */
    int                  ld_status;         /**< Return status at the end of
                                              * the execution.
                                              */
    struct dss_handle    ld_dss;            /**< per thread DSS handle for media
                                              * and device requests
                                              */
};

/** Elements pushed into the tosync_array of a device */
struct request_tosync {
    struct req_container *reqc;
    size_t medium_index; /**< index of the medium in the tosync_media array of
                           *  the release_params struct of the reqc
                           */
};

/**
 * Parameters to check when a synchronization is required.
 */
struct sync_params {
    GPtrArray       *tosync_array;  /**< array of release requests with to_sync
                                      *  set
                                      */
    struct timespec  oldest_tosync; /**< oldest release request in
                                      *  \p tosync_array
                                      */
    size_t           tosync_size;   /**< total size of release requests in
                                      *  \p tosync_array
                                      */
};

/**
 * Data specific to the device thread.
 */
struct lrs_dev {
    pthread_mutex_t      ld_mutex;              /**< exclusive access */
    struct dev_info     *ld_dss_dev_info;       /**< device info from DSS */
    struct lib_drv_info  ld_lib_dev_info;       /**< device info from library
                                                  *  (for tape drives)
                                                  */
    struct ldm_dev_state ld_sys_dev_state;      /**< device info from system */

    enum dev_op_status   ld_op_status;          /**< operational status of the
                                                  * device
                                                  */
    char                 ld_dev_path[PATH_MAX]; /**< path to the device */
    struct media_info   *ld_dss_media_info;     /**< loaded media info
                                                  *  from DSS, if any
                                                  */
    char                 ld_mnt_path[PATH_MAX]; /**< mount path of the
                                                  * filesystem
                                                  */
    struct req_container   *ld_format_request;  /**< format request to handle */
    bool                 ld_ongoing_io;         /**< one I/O is ongoing */
    bool                 ld_needs_sync;         /**< medium needs to be sync */
    struct thread_info   ld_device_thread;      /**< thread handling the actions
                                                  * executed on the device
                                                  */
    struct sync_params   ld_sync_params;        /**< pending synchronization
                                                  * requests
                                                  */
    struct tsqueue      *ld_response_queue;     /**< reference to the response
                                                  * queue
                                                  */
    struct format_media *ld_ongoing_format;     /**< reference to the ongoing
                                                  * format array
                                                  */
    struct tsqueue      *sched_req_queue;       /**< reference to the sched
                                                  * request queue
                                                  */
    struct lrs_dev_hdl  *ld_handle;
};

/**
 *  TODO: will become a device thread static function when all media operations
 *  will be moved to device thread
 */
int clean_tosync_array(struct lrs_dev *dev, int rc);

/**
 * Add a new sync request to a device
 *
 * \param[in,out]   dev     device to add the sync request
 * \param[in]       reqc    sync request to add
 * \param[in]       medium  index in reqc of the medium to sync
 *
 * \return                0 on success, -errno on failure
 */
int push_new_sync_to_device(struct lrs_dev *dev, struct req_container *reqc,
                            size_t medium_index);

/**
 * Initialize an lrs_dev_hdl to manipulate devices from the scheduler
 *
 * \param[out]   handle   pointer to an uninitialized handle
 * \param[in]    family   family of the devices handled
 *
 * \return                0 on success, -errno on failure
 */
int lrs_dev_hdl_init(struct lrs_dev_hdl *handle, enum rsc_family family);

/**
 * Undo the work done by lrs_dev_hdl_init
 *
 * \param[in]    handle   pointer to an initialized handle
 */
void lrs_dev_hdl_fini(struct lrs_dev_hdl *handle);

/**
 * Creates a new device thread and add it to the list of registered devices
 *
 * \param[in]    sched    scheduler managing the device
 * \param[in]    handle   initialized device handle
 * \param[in]    name     serial number of the device
 *
 * \return                0 on success, -errno no failure
 */
int lrs_dev_hdl_add(struct lrs_sched *sched,
                    struct lrs_dev_hdl *handle,
                    const char *name);

/**
 * Undo the work done by lrs_dev_hdl_add
 *
 * This function is blocking as it waits for the end of the device thread.
 *
 * \param[in]    handle   initialized device handle
 * \param[in]    index    index of the device to remove from the list
 * \param[in]    rc       error which caused the thread to stop
 *
 * \return                0 on success, -errno no failure
 */
int lrs_dev_hdl_del(struct lrs_dev_hdl *handle, int index, int rc);

/**
 * Load all the devices that are attributed to this LRS from the DSS
 *
 * \param[in]      sched    scheduler managing the device
 * \param[in/out]  handle   initialized device handle
 *
 * \return                0 on success, -errno no failure
 */
int lrs_dev_hdl_load(struct lrs_sched *sched,
                     struct lrs_dev_hdl *handle);

/**
 * Remove all the devices from the handle
 *
 * This function is blocking as it waits for the termination of all threads.
 * Each thread is signaled first and then joined so that they are stopped
 * concurrently.
 *
 * \param[in]  handle  pointer to an initialized handle to clear
 */
void lrs_dev_hdl_clear(struct lrs_dev_hdl *handle);

/**
 * Wrapper arround GLib's getter to retrive devices' structures
 *
 * \param[in]  handle  initialized device handle
 * \param[in]  index   index of the device, must be smaller than the number of
 *                     devices in handle->ldh_devices
 *
 * \return             a pointer to the requested device is returned
 */
struct lrs_dev *lrs_dev_hdl_get(struct lrs_dev_hdl *handle, int index);

/**
 * Signal to the device thread that it should stop working
 *
 * \param[in]  device  the device to signal
 */
void dev_thread_signal_stop(struct lrs_dev *device);

/**
 * Set the error status on device thread and signal that it should stop working
 *
 * \param[in]   device      device to signal
 * \param[in]   error_code  error code
 */
void dev_thread_signal_stop_on_error(struct lrs_dev *device, int error_code);

/**
 * Signal to the device thread that work has been received
 *
 * \param[in]  device  the device to signal
 */
void dev_thread_signal(struct lrs_dev *device);

/**
 * Wait for the termination of the device thread
 *
 * dev_thread_signal_stop must be called before this function as this one
 * is blocking.
 *
 * \param[in]  device  the device whose termination to wait for
 */
void dev_thread_wait_end(struct lrs_dev *device);

/**
 * Wrap library open operations
 *
 * @param[in]   dev_type    Device type
 * @param[out]  lib         Library handler.
 *
 * @return          0 on success, -1 * posix error code on failure.
 */
int wrap_lib_open(enum rsc_family dev_type, struct lib_adapter *lib);

#endif
