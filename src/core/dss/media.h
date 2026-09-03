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
 * \brief  Media resource header of Phobos's Distributed State Service.
 */

#ifndef _PHO_DSS_MEDIA_H
#define _PHO_DSS_MEDIA_H

#include "resources.h"

/**
 * The "media" operations structure.
 * Implements every function of the structure.
 */
extern const struct dss_resource_ops media_ops;

/**
 * Decode the stats of a medium from a given \p json.
 *
 * \param[out] stats  The stats in which to store the decoded JSON values
 * \param[in]  json   The JSON string to decode
 *
 * \return 0 on success, -EINVAL if \p json is not a valid JSON object
 *                       negative error code otherwise
 */
int dss_media_stats_decode(struct media_stats *stats, const char *json);

#endif
