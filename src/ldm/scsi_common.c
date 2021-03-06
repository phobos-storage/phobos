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
 * \brief  SCSI command helper
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "scsi_common.h"
#include "pho_common.h"

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>
#include <scsi/sg_io_linux.h>
#include <scsi/scsi.h>
#include <assert.h>

#define SCSI_MSG_LEN    256

/** Convert internal direction to SG equivalent */
static inline int scsi_dir2sg(enum scsi_direction direction)
{
    switch (direction) {
    case SCSI_GET: return SG_DXFER_FROM_DEV;
    case SCSI_PUT: return SG_DXFER_TO_DEV;
    case SCSI_NONE: return SG_DXFER_NONE;
    }
    return -1;
}

/** Convert SCSI host_status to -errno code */
static int scsi_host_status2errno(uint16_t host_status)
{
    switch (host_status) {
    case SG_LIB_DID_OK:           return 0;
    case SG_LIB_DID_NO_CONNECT:   return -ECONNABORTED;
    case SG_LIB_DID_BUS_BUSY:     return -EBUSY;
    case SG_LIB_DID_TIME_OUT:     return -ETIMEDOUT;
    case SG_LIB_DID_BAD_TARGET:   return -EINVAL;
    case SG_LIB_DID_ABORT:        return -ECANCELED;
    case SG_LIB_DID_PARITY:       return -EIO;
    case SG_LIB_DID_ERROR:        return -EIO;
    case SG_LIB_DID_RESET:        return -ECANCELED;
    case SG_LIB_DID_BAD_INTR:     return -EINTR;
    case SG_LIB_DID_PASSTHROUGH:  return -EIO; /* ? */
    case SG_LIB_DID_SOFT_ERROR:   return -EAGAIN;
    case SG_LIB_DID_IMM_RETRY:    return -EAGAIN; /* retry immediately */
    case SG_LIB_DID_REQUEUE:      return -EBUSY;  /* need retry after a while */
    default:                      return -EIO;
    }
}

/** Convert Sense Key to -errno code */
static int scsi_sense_key2errno(uint8_t sense_key)
{
    switch (sense_key) {
    case  SPC_SK_NO_SENSE:
        return 0;

    case  SPC_SK_RECOVERED_ERROR:
    case  SPC_SK_UNIT_ATTENTION:
        return -EAGAIN;

    case  SPC_SK_NOT_READY:
        return -EBUSY;

    case  SPC_SK_ILLEGAL_REQUEST:
        return -EINVAL;

    case  SPC_SK_DATA_PROTECT:
        return -EPERM;

    case  SPC_SK_BLANK_CHECK:
    case  SPC_SK_COPY_ABORTED:
    case  SPC_SK_ABORTED_COMMAND:
    case  SPC_SK_VOLUME_OVERFLOW:
    case  SPC_SK_MISCOMPARE:
    case  SPC_SK_MEDIUM_ERROR:
    case  SPC_SK_HARDWARE_ERROR:
    default:
        return -EIO;
    }
}

/** Convert SCSI masked_status to -errno code */
static int scsi_masked_status2errno(uint8_t masked_status)
{
    switch (masked_status) {
    case GOOD:
    case CONDITION_GOOD:
    case INTERMEDIATE_GOOD:
    case INTERMEDIATE_C_GOOD:
        return 0;
    case BUSY:
    case RESERVATION_CONFLICT:
    case QUEUE_FULL:
        return -EBUSY;
    case COMMAND_TERMINATED:
    case CHECK_CONDITION:
    default:
        return -EIO;
    }
}

/** check if the SCSI request was errorneous */
static inline bool scsi_error_check(const struct sg_io_hdr *hdr)
{
    assert(hdr != NULL);

    return (hdr->masked_status != 0 || hdr->host_status != 0
        || hdr->driver_status != 0);
}

static void scsi_error_trace(const struct sg_io_hdr *hdr)
{
    const struct scsi_req_sense *sbp;
    char msg[SCSI_MSG_LEN];

    assert(hdr != NULL);

    pho_warn("SCSI ERROR: scsi_masked_status=%#hhx, adapter_status=%#hx, "
             "driver_status=%#hx", hdr->masked_status,
             hdr->host_status, hdr->driver_status);

    sbp = (const struct scsi_req_sense *)hdr->sbp;
    if (sbp == NULL) {
        pho_warn("sbp=NULL");
        return;
    }
    pho_warn("    req_sense_error=%#hhx, sense_key=%#hhx (%s)",
             sbp->error_code, sbp->sense_key,
             sg_get_sense_key_str(sbp->sense_key, sizeof(msg), msg));
    pho_warn("    asc=%#hhx, ascq=%#hhx (%s)", sbp->additional_sense_code,
             sbp->additional_sense_code_qualifier,
             sg_get_asc_ascq_str(sbp->additional_sense_code,
                sbp->additional_sense_code_qualifier, sizeof(msg), msg));
}

int scsi_execute(int fd, enum scsi_direction direction,
                 unsigned char *cdb, int cdb_len,
                 struct scsi_req_sense *sbp, int sb_len,
                 void *dxferp, int dxfer_len,
                 unsigned int timeout_msec)
{
    int rc;
    struct sg_io_hdr hdr = {0};

    hdr.interface_id = 'S'; /* S for generic SCSI */
    hdr.dxfer_direction = scsi_dir2sg(direction);
    hdr.cmdp = cdb;
    hdr.cmd_len = cdb_len;
    hdr.sbp = (unsigned char *)sbp;
    hdr.mx_sb_len = sb_len;
    /* hdr.iovec_count = 0 implies no scatter gather */
    hdr.dxferp = dxferp;
    hdr.dxfer_len = dxfer_len;
    hdr.timeout = timeout_msec;
    /* hdr.flags = 0: default */

    rc = ioctl(fd, SG_IO, &hdr);
    if (rc)
        LOG_RETURN(rc = -errno, "ioctl() failed");

    if (scsi_error_check(&hdr))
        scsi_error_trace(&hdr);

    if (hdr.masked_status == CHECK_CONDITION) {
        /* check sense_key value */
        rc = scsi_sense_key2errno(sbp->sense_key);
        if (rc)
            LOG_RETURN(rc, "Sense key %#hhx (converted to %d)", sbp->sense_key,
                       rc);
    } else {
        rc = scsi_masked_status2errno(hdr.masked_status);
        if (rc)
            LOG_RETURN(rc, "SCSI error %#hhx (converted to %d)",
                       hdr.masked_status, rc);
    }

    rc = scsi_host_status2errno(hdr.host_status);
    if (rc)
        LOG_RETURN(rc, "Adapter error %#hx (converted to %d)", hdr.host_status,
                   rc);
    return 0;
}
