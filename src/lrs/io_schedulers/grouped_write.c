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
 */

static int gw_init(struct io_scheduler *io_sched)
{
    return 0;
}

static void gw_fini(struct io_scheduler *io_sched)
{
}

static int gw_peek_request(struct io_scheduler *io_sched,
                           struct req_container **reqc)
{
    return 0;
}

static int gw_push_request(struct io_scheduler *io_sched,
                           struct req_container *reqc)
{
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
