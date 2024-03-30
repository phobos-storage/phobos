/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2024 CEA/DAM.
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
 * \brief  Filter header of Phobos's Distributed State Service.
 */

#ifndef _PHO_DSS_FILTERS_H
#define _PHO_DSS_FILTERS_H

#include <gmodule.h>
#include <stdbool.h>

#include "pho_dss.h"

static inline bool key_is_logical_op(const char *key)
{
    return !g_ascii_strcasecmp(key, "$AND") ||
           !g_ascii_strcasecmp(key, "$NOR") ||
           !g_ascii_strcasecmp(key, "$OR")  ||
           !g_ascii_strcasecmp(key, "$NOT");
}

int clause_filter_convert(struct dss_handle *handle, GString *qry,
                          const struct dss_filter *filter);

#endif
