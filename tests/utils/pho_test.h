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

#ifndef PHO_TEST_H
#define PHO_TEST_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <getopt.h>
#include <lrs_cfg.h>
#include <phobos_store.h>
#include <pho_cfg.h>
#include <pho_comm.h>
#include <pho_srl_lrs.h>
#include <pho_types.h>
#include <pho_type_utils.h>
#include <sys/epoll.h>

#define pho_exit(err, fmt, ...)                                       \
    do {                                                              \
        error(EXIT_FAILURE, abs(err), fmt __VA_OPT__(,) __VA_ARGS__); \
        __builtin_unreachable();                                      \
    } while (0)

struct pho_comm_info *phobosd_connect(void);

void phobosd_disconnect(struct pho_comm_info *comm);

void send_request(struct pho_comm_info *comm,
                  pho_req_t *req);

void recv_responses(struct pho_comm_info *comm,
                    pho_resp_t ***resps,
                    int *n_resps);

pho_req_t *make_read_request(int id, int n_media, int n_required,
                             enum rsc_family family,
                             const char **medium_name,
                             const char *library);

pho_req_t *make_write_request(int id, int n_media, int size,
                              enum rsc_family family,
                              const char *grouping,
                              struct string_array *tags,
                              const char *library);

pho_req_t *make_release_request(const pho_resp_t *resp, const pho_req_t *walloc,
                                int id, bool async);

#endif
