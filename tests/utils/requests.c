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

#include "pho_test.h"

struct pho_comm_info *phobosd_connect(void)
{
    union pho_comm_addr addr = {0};
    struct pho_comm_info *comm;
    int rc;

    comm = xmalloc(sizeof(*comm));

    addr.af_unix.path = PHO_CFG_GET(cfg_lrs, PHO_CFG_LRS, server_socket);
    rc = pho_comm_open(comm, &addr, PHO_COMM_UNIX_CLIENT);
    if (rc)
        pho_exit(rc, "failed to open phobosd socket");

    return comm;
}

void phobosd_disconnect(struct pho_comm_info *comm)
{
    pho_comm_close(comm);
}

void send_request(struct pho_comm_info *comm,
                  pho_req_t *req)
{
    struct pho_comm_data data;
    int rc;

    data = pho_comm_data_init(comm);
    pho_srl_request_pack(req, &data.buf);

    rc = pho_comm_send(&data);
    if (rc)
        pho_exit(rc, "failed to send request");

    free(data.buf.buff);
}

void recv_responses(struct pho_comm_info *comm,
                    pho_resp_t ***resps,
                    int *n_resps)
{
    struct pho_comm_data *responses = NULL;
    int rc;
    int i;

    *n_resps = 0;
    *resps = NULL;
    rc = pho_comm_recv(comm, &responses, n_resps);
    if (rc == -EWOULDBLOCK || rc == -EAGAIN) {
        /* FIXME a better approach would be an epoll based client loop
         * but this is not currently possible with the client API.
         * This prevents the comm_thread from using too much CPU
         * resources when there is nothing to read on the socket.
         */
        sleep(1);
        return;
    }
    if (rc)
        pho_exit(rc, "failed to receive response");
    if (*n_resps == 0) {
        sleep(1);
        return;
    }

    *resps = xmalloc(*n_resps * sizeof(*resps));

    for (i = 0; i < *n_resps; i++)
        (*resps)[i] = pho_srl_response_unpack(&responses[0].buf);

    free(responses);
}

pho_req_t *make_read_request(int id, int n_media, int n_required,
                             enum rsc_family family,
                             const char **medium_name,
                             const char *library)
{
    pho_req_t *req;
    int i;

    req = xmalloc(sizeof(*req));
    pho_srl_request_read_alloc(req, n_media);
    req->id = id;
    req->ralloc->n_required = n_required;
    for (i = 0; i < n_media; i++) {
        req->ralloc->med_ids[i]->name = xstrdup(medium_name[i]);
        req->ralloc->med_ids[i]->library = xstrdup_safe(library);
        req->ralloc->med_ids[i]->family = (PhoResourceFamily)family;
    }

    return req;
}

pho_req_t *make_write_request(int id, int n_media, int size,
                              enum rsc_family family,
                              const char *grouping,
                              struct string_array *tags,
                              const char *library)
{
    size_t *n_tags = xmalloc(tags->count * sizeof(*n_tags));
    pho_req_t *req = xmalloc(sizeof(*req));
    int i;

    for (i = 0; i < n_media; i++)
        n_tags[i] = tags->count;

    pho_srl_request_write_alloc(req, n_media, n_tags);
    req->id = id;
    req->walloc->library = xstrdup_safe(library);
    req->walloc->family = (PhoResourceFamily)family;
    req->walloc->grouping = xstrdup_safe(grouping);
    req->walloc->n_media = n_media;
    for (i = 0; i < n_media; i++) {
        size_t j;

        req->walloc->media[i]->empty_medium = false;
        req->walloc->media[i]->size = size;

        for (j = 0; j < tags->count; j++)
            req->walloc->media[i]->tags[j] = tags->strings[j];
    }

    return req;
}

#define resp_medium(resp, i)    \
    pho_response_is_read(resp) ?         \
        resp->ralloc->media[i]->med_id : \
        resp->walloc->media[i]->med_id

pho_req_t *make_release_request(const pho_resp_t *resp, const pho_req_t *walloc,
                                int id, bool async)
{
    bool is_read = pho_response_is_read(resp);
    pho_req_t *req;
    size_t n_media;
    size_t i;

    assert(is_read || pho_response_is_write(resp));

    req = xmalloc(sizeof(*req));

    n_media = is_read ? resp->ralloc->n_media : resp->walloc->n_media;
    pho_srl_request_release_alloc(req, n_media, is_read);
    req->id = id;

    for (i = 0; i < n_media; i++) {
        PhoResourceId *medium = resp_medium(resp, i);

        req->release->media[i]->med_id->library = xstrdup(medium->library);
        req->release->media[i]->med_id->family = medium->family;
        req->release->media[i]->med_id->name = xstrdup(medium->name);
        req->release->media[i]->rc = 0;
        req->release->media[i]->to_sync = async ? false : !is_read;
        req->release->media[i]->size_written = is_read ?
            0 : walloc->walloc->media[0]->size;
        req->release->media[i]->nb_extents_written = is_read ? 0 : 1;
    }

    return req;
}
