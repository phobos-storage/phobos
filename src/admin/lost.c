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
 * \brief  Phobos admin source file for removal of lost media
 */

#include "pho_types.h"

#include "import.h"
#include "lost.h"

static int get_extents_from_medium(struct dss_handle *dss,
                                   struct pho_id *medium_id,
                                   struct extent **extents,
                                   int *extent_count)
{
    struct dss_filter filter;
    int rc;

    rc = dss_filter_build(&filter,
                          "{\"$AND\": ["
                          "  {\"DSS::EXT::medium_family\": \"%s\"},"
                          "  {\"DSS::EXT::medium_id\": \"%s\"},"
                          "  {\"DSS::EXT::medium_library\": \"%s\"}"
                          "]}",
                          rsc_family2str(medium_id->family),
                          medium_id->name,
                          medium_id->library);
    if (rc)
        LOG_RETURN(rc, "Failed to build filter for extent retrieval");

    rc = dss_extent_get(dss, &filter, extents, extent_count);
    dss_filter_free(&filter);
    if (rc)
        pho_error(rc,
                  "Failed to retrieve (family '%s', name '%s', library '%s') extents",
                  rsc_family2str(medium_id->family), medium_id->name,
                  medium_id->library);

    return rc;
}

static int get_layout_from_extent(struct dss_handle *dss,
                                  struct extent *extent,
                                  struct layout_info **layout)
{
    struct dss_filter filter;
    int layout_count;
    int rc;

    rc = dss_filter_build(&filter, "{\"DSS::LYT::extent_uuid\": \"%s\"}",
                          extent->uuid);
    if (rc)
        LOG_RETURN(rc, "Failed to build filter for layout retrieval");

    rc = dss_layout_get(dss, &filter, layout, &layout_count);
    dss_filter_free(&filter);

    assert(layout_count == 1);

    return rc;
}

int delete_media_and_extents(struct admin_handle *handle,
                             struct media_info *media_list,
                             int media_count)
{
    int rc;
    int i;
    int j;

    for (i = 0; i < media_count; i++) {
        struct media_info *medium = &media_list[i];
        struct extent *extents;
        int extent_count;

        rc = get_extents_from_medium(&handle->dss, &medium->rsc.id, &extents,
                                     &extent_count);
        if (rc)
            return rc;

        for (j = 0; j < extent_count; j++) {
            struct layout_info *layout;
            struct copy_info copy;

            rc = get_layout_from_extent(&handle->dss, &extents[j], &layout);
            if (rc)
                return rc;

            copy.object_uuid = layout->uuid;
            copy.version = layout->version;
            copy.copy_name = layout->copy_name;

            rc = dss_layout_delete(&handle->dss, layout, 1);
            if (rc)
                return rc;

            rc = reconstruct_copy(handle, &copy);
            if (rc)
                return rc;

            dss_res_free(layout, 1);
        }
    }

    return 0;
}
