/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2019 CEA/DAM.
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
 * \brief  Phobos Local Resource Scheduler (LRS) - scheduler
 */
#ifndef _PHO_LRS_SCHED_H
#define _PHO_LRS_SCHED_H

#include <stdint.h>

#include "pho_dss.h"
#include "pho_srl_lrs.h"
#include "pho_types.h"

struct dev_descr;

/**
 * Local Resource Scheduler instance, manages media and local devices for the
 * actual IO to be performed.
 */
struct lrs_sched {
    struct dss_handle  dss;             /**< Associated DSS */
    enum rsc_family    family;          /**< Managed resource family */
    GArray            *devices;         /**< List of available devices */
    const char        *lock_hostname;   /**< Lock hostname for this LRS */
    int                lock_owner;      /**< Lock owner (pid) for this LRS */
    GQueue            *req_queue;       /**< Queue for all but
                                          *  release requests
                                          */
    GQueue            *release_queue;   /**< Queue for release requests */
    GQueue            *response_queue;  /**< Queue for release responses */
};

/**
 * Enumeration for request/medium synchronization status.
 */
enum tosync_status {
    SYNC_TODO,                      /**< Synchronization is requested */
    SYNC_DONE,                      /**< Synchronization is done */
    SYNC_ERROR,                     /**< Synchronization failed */
};

/**
 * Internal structure for a to-synchronize medium couple.
 */
struct tosync_medium {
    enum tosync_status status;      /**< Medium synchronization status. */
    struct pho_id device;           /**< Device ID. */
    struct pho_id medium;           /**< Medium ID. */
    size_t written_size;            /**< Written size on the medium to sync. */
};

/**
 * Request container used by the scheduler to transfer information
 * between a request and its response. For now, this information consists
 * in a socket ID.
 */
struct req_container {
    int socket_id;                  /**< Socket ID to pass to the response. */
    pho_req_t *req;                 /**< Request. */
    union {                         /**< Parameters used by the LRS. */
        struct {
            struct tosync_medium *tosync_media;
                                    /**< To-synchronize media. */
            int num_tosync_media;   /**< Number of media to synchronize. */
            int rc;                 /**< Global return code, if multiple sync
                                      *  failed, only the first one is kept.
                                      */
        } release;
    } params;
};

/**
 * Release memory allocated for params structure of a request container.
 */
static inline void destroy_container_params(struct req_container *cont)
{
    if (pho_request_is_release(cont->req))
        free(cont->params.release.tosync_media);
}

/**
 * Response container used by the scheduler to transfer information
 * between a request and its response. For now, this information consists
 * in a socket ID.
 */
struct resp_container {
    int socket_id;                  /**< Socket ID got from the request. */
    pho_resp_t *resp;               /**< Response. */
};

/**
 * Initialize a new sched bound to a given DSS.
 *
 * \param[in]       sched       The sched to be initialized.
 * \param[in]       family      Resource family managed by the scheduler.
 *
 * \return                      0 on success, -1 * posix error code on failure.
 */
int sched_init(struct lrs_sched *sched, enum rsc_family family);

/**
 * Free all resources associated with this sched except for the dss, which must
 * be deinitialized by the caller if necessary.
 *
 * \param[in]       sched       The sched to be deinitialized.
 */
void sched_fini(struct lrs_sched *sched);

/**
 * Enqueue a request to be handled by the sched. The request is guaranteed to be
 * answered at some point.
 *
 * The request is encapsulated in a container which owns a socket ID that must
 * be given back with the associated response.
 *
 * \param[in]       sched       The sched that will handle the request.
 * \param[in]       reqc        The request to enqueue.
 *
 * \return                      0 on success, -1 * posix error code on failure.
 */
int sched_request_enqueue(struct lrs_sched *sched, struct req_container *reqc);

/**
 * Get responses for all handled pending requests (enqueued with
 * sched_request_enqueue).
 *
 * The responses are encapsulated in a container which owns a socket ID given by
 * the associated request.
 *
 * \param[in]       sched       The sched from which to get the responses.
 * \param[out]      n_resp      The number of responses generated by the sched.
 * \param[out]      respc       The responses issued from the sched process.
 *
 * \return                      0 on success, -1 * posix error code on failure.
 */
int sched_responses_get(struct lrs_sched *sched, int *n_resp,
                        struct resp_container **respc);

#endif
