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
 * \brief   Phobos communication interface.
 */
#ifndef _PHO_COMM_H
#define _PHO_COMM_H

#include <glib.h>

#include "pho_types.h"

/**
 * Data structure used to store communication information needed by this API.
 * This structure is initialized using pho_comm_open() and cleaned
 * using pho_comm_close().
 */
struct pho_comm_info {
    bool is_server;     /*!< true if the caller is a server,
                         *   false if a client.
                         */
    char *path;         /*!< Socket path. */
    int socket_fd;      /*!< Main socket descriptor
                         *   (the one open with socket() call).
                         */
    int epoll_fd;       /*!< Socket poll descriptor (used by the server). */
    GHashTable *ev_tab; /*!< Hash table of events of the socket poll
                         *   (used by the server for cleaning).
                         */
};

/**
 * Initializer for the communication information data structure where
 * .socket_fd is initialized to -1, and gets the correct behavior if
 * pho_comm_close() is called before pho_comm_open().
 *
 * It is the caller responsability to call it if pho_comm_close() may be
 * called before pho_comm_open().
 *
 * \return                      An initialized data structure.
 */
static inline struct pho_comm_info pho_comm_info_init(void)
{
    struct pho_comm_info info = {
        .is_server = false,
        .path = NULL,
        .socket_fd = -1,
        .epoll_fd = -1,
        .ev_tab = NULL
    };

    return info;
}

/** Data structure used to send and receive messages. */
struct pho_comm_data {
    int fd;             /*!< Socket descriptor where the msg comes from/to. */
    struct pho_buff buf;/*!< Message contents. */
};

/**
 * Initializer for the send & receive data structure where .fd is initialized
 * to the ci.socket_fd and the buffer is empty.
 *
 * It is the caller responsability to call it if .fd is not directly assigned,
 * which may lead to a socket error during sendings.
 *
 * \param[in]       ci          Communication info.
 * \return                      An initialized data structure.
 */
static inline struct pho_comm_data pho_comm_data_init(struct pho_comm_info *ci)
{
    struct pho_comm_data dt = {
        .fd = ci->socket_fd,
        .buf.size = 0,
        .buf.buff = NULL
    };

    return dt;
}

/**
 * Opener for the unix socket.
 *
 * \param[out]      ci          Communication info to be initialized.
 * \param[in]       sock_path   Socket path.
 * \param[in]       is_server   true if the opening was requested by the server,
 *                              false otherwise.
 *
 * \return                      0 on success, -errno on failure.
 */
int pho_comm_open(struct pho_comm_info *ci, const char *sock_path,
                  const bool is_server);

/**
 * Closer for the unix socket.
 *
 * The communication info structure will be cleaned.
 *
 * \param[in]       ci          Communication info.
 *
 * \return                      0 on success, -errno on failure.
 */
int pho_comm_close(struct pho_comm_info *ci);

/**
 * Send a message through the unix socket provided in data.
 *
 * \param[in]       data        Message data to send.
 *
 * \return                      0 on success, -errno on failure.
 */
int pho_comm_send(const struct pho_comm_data *data);

/**
 * Receive a message from the unix socket.
 *
 * The client receives one message per call.
 * The server will check its socket poll and receive all the available
 * messages ie. process the accept/close requests and retrieve the contents
 * sent by the clients.
 * The caller has to free the data array and each data contents (buffers).
 *
 *
 * \param[in]       ci          Communication info.
 * \param[out]      data        Received message data.
 * \param[out]      nb_data     Number of received message data (possibly 0).
 *
 * \return                      0 on succes, -errno on failure.
 */
int pho_comm_recv(struct pho_comm_info *ci, struct pho_comm_data **data,
                  int *nb_data);

#endif
