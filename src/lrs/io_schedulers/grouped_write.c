/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2025 CEA/DAM.
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
 * \brief  LRS Grouped Write I/O Scheduler: group write request per tag and/or groupings.
 */
#include "lrs_sched.h"
#include "lrs_utils.h"
#include "pho_cfg.h"
#include "pho_common.h"
#include "pho_types.h"
#include "schedulers.h"

/* Principle of the algorithm:
 *
 * The goal is to group write requests per tag and RAID parameters (e.g.
 * n_media). The assumption is that upper layers will be smart enough to push
 * writes to the LRS so that by performing them in order the devices will be
 * used most of the time. For instance, one device won't stay idle waiting for
 * a second device to be available for a RAID1/repl_count=2 write.
 *
 * We also group writes per grouping on the tapes. This will increase data
 * locality to reduce the read latency. Less tape seeks will be necessary to
 * read all the data of a given grouping.
 *
 * To achieve this, requests are put in queues (struct gw_queue) that are stored
 * in an hashtable. They are indexed by layout parameters and tags. Each
 * gw_queue contains a list of queues. One queue per grouping. When new requests
 * are pushed, the appropriate queue is retrieved from the hash table using
 * the request's parameters. The request is then inserted in the appropriate
 * grouping's queue.
 *
 * One assumption made by this algorithm is that all the media of a given
 * write request all use the same tags. The protocol seems to support
 * different tags per medium. This is not currently the case in Phobos.
 */

#define walloc_grouping(reqc) \
    (reqc)->req->walloc->grouping

struct gw_request {
    struct req_container *reqc;
    /** Pointer to the queue containing this request */
    struct gw_queue *queue;
};

struct gw_grouping {
    /** Name of the grouping used as the key in gw_queue::grouping_index */
    char *name;
    /** List of struct gw_request for this grouping */
    GQueue *requests;
};

/**
 * All the requests in an instance of this structure must have the same tags.
 * Requests are grouped and scheduled by number of drives required (e.g. RAID
 * params).
 *
 * The queues are created when requests with new tags+RAID params are received
 * and free'd when we need to switch queue. We can only switch queue when the
 * list is empty.
 */
struct gw_queue {
  /** Type of layout (e.g. RAID1, RAID4).
   * For now, this will always be 0 but can be used in the future to
   * differentiate between RAID1 repl_count=3 and RAID4.
   */
  int layout_type;
  /** Number of media per request */
  gint64 n_media;
  /** comma seperated list of tags */
  const char *tags;

  /** List of struct gw_grouping in the order of each groupings' first request
   * received by the scheduler. This is used to write groupings in order.
   */
  GQueue *groupings;
  /** map grouping name to struct gw_grouping for fast lookup */
  GHashTable *grouping_index;
  /** Special case of requests without grouping that can't be stored in the hash
   * table.
   */
  struct gw_grouping *no_grouping;

  /** List of size n_media. Devices used for these requests */
  struct lrs_dev **devices;
};

struct gw_state {
    /** key = value = struct gw_queue.
     *
     * gw_queue is hashed based on the layout type, number of media per request and tag
     * list. This information is contained in the struct gw_queue. There is no external
     * key. Therefore, gw_queue is the key and the value.
     */
    GHashTable *queues;

    /**
     * List of queues in \p queues added in the order they are created
     * to match the order in which they are sent to the LRS.
     */
    GList *ordered_queues;

    /**
     * List of queues allocated to devices.
     */
    GList *allocated;

    /** Request currently being allocated (peek -> get_device_medium_pair ->
     * remove/request cycle)
     */
    struct gw_request *current;

    /** List of devices eligible for scheduling a new queue */
    GPtrArray *free_devices;

    /** Hash table to quickly find a queue given a device. */
    GHashTable *device_to_queue;
};

static struct gw_grouping *gw_grouping_new(const char *name)
{
    struct gw_grouping *grouping;

    grouping = xmalloc(sizeof(*grouping));
    grouping->name = xstrdup_safe(name);
    grouping->requests = g_queue_new();

    return grouping;
}

static guint glib_wqueue_hash(gconstpointer v)
{
    const struct gw_queue *queue = v;

    return g_str_hash(queue->tags) ^ g_int64_hash(&queue->n_media) ^
        g_int_hash(&queue->layout_type);
}

static gboolean glib_wqueue_equal(gconstpointer _lhs, gconstpointer _rhs)
{
    const struct gw_queue *lhs = _lhs;
    const struct gw_queue *rhs = _rhs;

    return (lhs->layout_type == rhs->layout_type) &&
        (lhs->n_media == rhs->n_media) &&
        !strcmp(lhs->tags, rhs->tags);
}

static int gw_init(struct io_scheduler *io_sched)
{
    struct gw_state *state;
    int rc;

    state = xcalloc(1, sizeof(*state));
    /* explicit initialization even though it is already NULL just to show
     * that we have empty GLists
     */
    state->allocated = NULL;
    state->ordered_queues = NULL;
    state->queues = g_hash_table_new(glib_wqueue_hash, glib_wqueue_equal);
    if (!state->queues)
        GOTO(free_state, rc = -ENOMEM);

    state->device_to_queue = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!state->device_to_queue)
        GOTO(free_queues, rc = -ENOMEM);

    state->free_devices = g_ptr_array_new();
    if (!state->free_devices)
        GOTO(free_device_to_queue, rc = -ENOMEM);

    io_sched->private_data = state;

    return 0;

free_device_to_queue:
    g_hash_table_destroy(state->device_to_queue);
free_queues:
    g_hash_table_destroy(state->queues);
free_state:
    free(state);

    return rc;
}

static void gw_fini(struct io_scheduler *io_sched)
{
    struct gw_state *state = io_sched->private_data;

    g_ptr_array_free(state->free_devices, TRUE);
    g_hash_table_destroy(state->device_to_queue);
    g_hash_table_destroy(state->queues);
    free(state);
}

static char *tag2csv(pho_req_write_t *walloc)
{
    char *tag_list;
    size_t len = 0;
    size_t n_tags;
    char **tags;
    char *iter;
    size_t i;

    assert(walloc->n_media > 0);

    tags = walloc->media[0]->tags;
    n_tags = walloc->media[0]->n_tags;

    for (i = 0; i < n_tags; i++)
        len += strlen(tags[i]) + 1; // + 1 for the ','

    if (n_tags > 0)
        len--; // remove last +1 for ','

    tag_list = xmalloc(len + 1); // +1 for '\0'
    iter = tag_list;

    for (i = 0; i < n_tags; i++) {
        size_t len = strlen(tags[i]);

        memcpy(iter, tags[i], len);
        iter += len;
        *iter++ = ',';
    }
    if (n_tags > 0)
        iter--; // move iter back to overwrite last ','
    *iter = '\0';

    pho_debug("build tag_list '%s' for '%p'", tag_list, walloc);

    return tag_list;
}

static struct gw_queue *gw_find_queue(struct gw_state *state,
                                      struct req_container *reqc)
{
    struct gw_queue tmp = {
        .n_media = reqc->req->walloc->n_media,
        .layout_type = 0,
        .tags = tag2csv(reqc->req->walloc),
    };
    struct gw_queue *queue;

    queue = g_hash_table_lookup(state->queues, &tmp);
    free((void *)tmp.tags);

    return queue;
}

static struct gw_queue *gw_queue_create(struct gw_state *state,
                                        struct req_container *reqc)
{
    struct gw_queue *queue = xcalloc(1, sizeof(*queue));

    queue->tags = tag2csv(reqc->req->walloc);
    queue->n_media = reqc->req->walloc->n_media;
    queue->devices = xcalloc(queue->n_media, sizeof(*queue->devices));
    queue->groupings = g_queue_new();
    queue->grouping_index = g_hash_table_new(g_str_hash, g_str_equal);
    queue->layout_type = 0;

    g_hash_table_insert(state->queues, queue, queue);
    state->ordered_queues = g_list_append(state->ordered_queues, queue);

    return queue;
}

static struct gw_grouping *get_grouping(struct gw_queue *queue,
                                        const char *name)
{
    struct gw_grouping *grouping;

    if (!name) {
        if (queue->no_grouping)
            return queue->no_grouping;

        goto new_grouping;
    }

    grouping = g_hash_table_lookup(queue->grouping_index, name);
    if (grouping)
        return grouping;

new_grouping:
    grouping = gw_grouping_new(name);
    g_queue_push_head(queue->groupings, grouping);
    if (name)
        g_hash_table_insert(queue->grouping_index, grouping->name, grouping);
    else
        queue->no_grouping = grouping;

    return grouping;
}

static void gw_queue_push(struct gw_queue *queue, struct req_container *reqc)
{
    struct gw_request *req = xmalloc(sizeof(*req));
    struct gw_grouping *grouping;

    req->reqc = reqc;
    req->queue = queue;

    grouping = get_grouping(queue, walloc_grouping(reqc));
    assert(grouping);

    g_queue_push_tail(grouping->requests, req);
}

static int gw_push_request(struct io_scheduler *io_sched,
                           struct req_container *reqc)
{
    struct gw_state *state = io_sched->private_data;
    struct gw_queue *queue;

    assert(pho_request_is_write(reqc->req));

    queue = gw_find_queue(state, reqc);
    if (!queue)
        queue = gw_queue_create(state, reqc);

    gw_queue_push(queue, reqc);

    return 0;
}

static int gw_remove_request(struct io_scheduler *io_sched,
                             struct req_container *reqc)
{
    return 0;
}

static int gw_requeue(struct io_scheduler *io_sched,
                      struct req_container *reqc)
{
    return 0;
}

static int gw_peek_request(struct io_scheduler *io_sched,
                           struct req_container **reqc)
{
    return 0;
}

static int gw_get_device_medium_pair(struct io_scheduler *io_sched,
                                     struct req_container *reqc,
                                     struct lrs_dev **dev,
                                     size_t *index)
{
    return 0;
}

static int gw_retry(struct io_scheduler *io_sched,
                    struct sub_request *sreq,
                    struct lrs_dev **dev)
{
    return 0;
}

static void gw_add_device(struct io_scheduler *io_sched,
                          struct lrs_dev *new_device)
{
}

static struct lrs_dev **gw_get_device(struct io_scheduler *io_sched,
                                      size_t i)
{
    return 0;
}

static int gw_remove_device(struct io_scheduler *io_sched,
                            struct lrs_dev *device)
{
    return 0;
}

static int gw_claim_device(struct io_scheduler *io_sched,
                           enum io_sched_claim_device_type type,
                           union io_sched_claim_device_args *args)
{
    return 0;
}

struct io_scheduler_ops IO_SCHED_GROUPED_WRITE_OPS = {
    .init                   = gw_init,
    .fini                   = gw_fini,
    .push_request           = gw_push_request,
    .remove_request         = gw_remove_request,
    .requeue                = gw_requeue,
    .peek_request           = gw_peek_request,
    .get_device_medium_pair = gw_get_device_medium_pair,
    .retry                  = gw_retry,
    .add_device             = gw_add_device,
    .get_device             = gw_get_device,
    .remove_device          = gw_remove_device,
    .claim_device           = gw_claim_device,
};
