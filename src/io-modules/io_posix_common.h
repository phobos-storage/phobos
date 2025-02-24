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
 * \brief  Phobos I/O POSIX common adapter functions header.
 */
#ifndef _PHO_IO_POSIX_COMMON_H
#define _PHO_IO_POSIX_COMMON_H

#include "pho_io.h"
#include "pho_types.h"

struct posix_io_ctx {
    char *fpath;
    int fd;
};

int pho_posix_get(const char *extent_desc, struct pho_io_descr *iod);

int pho_posix_del(struct pho_io_descr *iod);

int pho_posix_open(const char *extent_desc, struct pho_io_descr *iod,
                   bool is_put);

int pho_posix_iod_from_fd(struct pho_io_descr *iod, int fd);

int pho_posix_write(struct pho_io_descr *iod, const void *buf, size_t count);

ssize_t pho_posix_read(struct pho_io_descr *iod, void *buf, size_t count);

int pho_posix_close(struct pho_io_descr *iod);

int pho_posix_set_md(const char *extent_desc, struct pho_io_descr *iod);

ssize_t pho_posix_preferred_io_size(struct pho_io_descr *iod);

int build_addr_path(const char *extent_key, const char *extent_desc,
                    struct pho_buff *addr);

char *full_xattr_name(const char *name);

int pho_getxattr(const char *path, int fd, const char *name, char **value);

int pho_get_common_xattrs_from_extent(struct pho_io_descr *iod,
                                      struct layout_info *lyt_info,
                                      struct extent *extent_to_insert,
                                      struct object_info *obj_info);

ssize_t pho_posix_size(struct pho_io_descr *iod);

#endif
