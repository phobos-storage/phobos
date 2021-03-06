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
 * \brief  Phobos Local Device Manager: dummy library.
 *
 * Dummy library for devices that are always online.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pho_ldm.h"
#include "pho_common.h"

/**
 * Return drive info for an online device.
 */
static int dummy_drive_lookup(struct lib_handle *lib, const char *drive_serial,
                            struct lib_drv_info *drv_info)
{
    const char  *sep = strchr(drive_serial, ':');
    ENTRY;

    if (sep == NULL)
        return -EINVAL;

    drv_info->ldi_addr.lia_type = MED_LOC_DRIVE;
    drv_info->ldi_addr.lia_addr = 0;
    drv_info->ldi_full = true;
    drv_info->ldi_medium_id.family = PHO_RSC_DIR; /** FIXME we don't care.
                                                   Could be disk or other... */
    return pho_id_name_set(&drv_info->ldi_medium_id, sep + 1);
}

/**
 * Extract path from drive identifier which consists of <host>:<path>.
 */

static int dummy_media_lookup(struct lib_handle *lib, const char *media_label,
                            struct lib_item_addr *med_addr)
{
    ENTRY;

    med_addr->lia_type = MED_LOC_DRIVE; /* always in drive */
    med_addr->lia_addr = 0;
    return 0;
}

/** Exported library adapater */
struct lib_adapter lib_adapter_dummy = {
    .lib_open  = NULL,
    .lib_close = NULL,
    .lib_drive_lookup = dummy_drive_lookup,
    .lib_media_lookup = dummy_media_lookup,
    .lib_media_move = NULL,
};
