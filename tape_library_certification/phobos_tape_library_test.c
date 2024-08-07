/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2023 CEA/DAM.
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

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "lrs_device.h"
#include "pho_cfg.h"
#include "pho_common.h"
#include "pho_ldm.h"
#include "pho_types.h"

#define NB_USECOND_SLEEP 10000 /* 10 ms */

const char *med_location_string[] = {"unknown", "drive", "slot", "arm",
                                     "impexp"};

static int device_load(const char *dev_serial, const char *tape_name)
{
    struct lib_handle lib_hdl;
    int rc;

    rc = wrap_lib_open(PHO_RSC_TAPE, "legacy", &lib_hdl);
    if (rc)
        LOG_RETURN(rc,
                   "Error when opening tape library module before loading tape "
                   "%s into drive %s", tape_name, dev_serial);

    rc = ldm_lib_load(&lib_hdl, dev_serial, tape_name);
    if (rc) {
        pho_error(rc, "Error when loading tape %s into drive %s",
                  tape_name, dev_serial);
        return rc;
    }

    rc = ldm_lib_close(&lib_hdl);
    if (rc) {
        pho_error(rc,
                  "Error when closing tape library handler after loading tape "
                  "%s into drive %s", tape_name, dev_serial);
        return rc;
    }

    return 0;
}


static int device_unload(const char *dev_serial, const char *tape_name)
{
    struct lib_handle lib_hdl;
    int rc;

    rc = wrap_lib_open(PHO_RSC_TAPE, "legacy", &lib_hdl);
    if (rc)
        LOG_RETURN(rc,
                   "Error when opening tape library module before unloading "
                   "tape %s from drive %s", tape_name, dev_serial);

    rc = ldm_lib_unload(&lib_hdl, dev_serial, tape_name);
    if (rc) {
        pho_error(rc,
                  "Error when unloading tape %s from drive %s",
                  tape_name, dev_serial);
        return rc;
    }

    rc = ldm_lib_close(&lib_hdl);
    if (rc) {
        pho_error(rc,
                  "Error when closing tape library handler after unloading "
                  "tape %s from drive %s", tape_name, dev_serial);
        return rc;
    }

    return 0;
}


struct dev_status {
    struct lib_item_addr dev_addr;
    char *tape_to_load_unload; /* NULL if device has no tape to load/unload */
    pthread_t thread_id;
    char *dev_name;
    bool failed;
};

static void *dev_load_unload(void *_dev_status)
{
    struct dev_status *dev_status = (struct dev_status *) _dev_status;
    int rc;

    pho_info("Device %s begins to load/unload the tape %s",
             dev_status->dev_name, dev_status->tape_to_load_unload);
    rc = device_load(dev_status->dev_name, dev_status->tape_to_load_unload);
    if (rc) {
        dev_status->failed = true;
        pho_error(rc, "Error device %s failed to load tape '%s'",
                  dev_status->dev_name, dev_status->tape_to_load_unload);
        pthread_exit(NULL);
    }

    rc = device_unload(dev_status->dev_name, dev_status->tape_to_load_unload);
    if (rc) {
        dev_status->failed = true;
        pho_error(rc, "Error device %s failed to unload tape '%s'",
                  dev_status->dev_name, dev_status->tape_to_load_unload);
        pthread_exit(NULL);
    }

    pho_info("Device %s successfully ends to load/unload the tape '%s'",
             dev_status->dev_name, dev_status->tape_to_load_unload);
    dev_status->tape_to_load_unload = NULL;
    pthread_exit(NULL);
}

int main(int argc, char **argv)
{
    struct dev_status *devices_status = NULL;
    int final_status = EXIT_SUCCESS;
    unsigned int nb_devices = 0;
    unsigned int nb_tapes = 0;
    struct lib_handle lib_hdl;
    char **tapes_name = NULL;
    char *tape_name;
    char *dev_name;
    int tape_index;
    int dev_index;
    char *saveptr;
    int rc;

    pho_context_init();
    atexit(pho_context_fini);

    if (argc != 3 && argc != 4) {
        fprintf(stderr,
                "usage : %s drives tapes [log_level]\n"
                "\n"
                "    drives:    comma separated list of drive serial numbers\n"
                "    tapes:     comma separated list of tape labels\n"
                "    log_level: integer from 0/DISABLED to 5/DEBUG,\n"
                "               (default is 3/INFO)\n",
                argv[0]);
        exit(EINVAL);
    }

    rc = pho_cfg_init_local(NULL);
    if (rc) {
        pho_error(rc, "Error when loading local conf");
        exit(-rc);
    }

    if (argc == 4)
        pho_log_level_set(atoi(argv[3]));

    rc = wrap_lib_open(PHO_RSC_TAPE, "legacy", &lib_hdl);
    if (rc) {
        pho_error(rc, "Error when opening tape library module");
        exit(-rc);
    }

    /* devices_by_lib_item_addr from conf */
    for (dev_name = strtok_r(argv[1], ",", &saveptr);
         dev_name != NULL;
         dev_name = strtok_r(NULL, ",", &saveptr)) {
        struct lib_drv_info dev_info;

        rc = ldm_lib_drive_lookup(&lib_hdl, dev_name, &dev_info);
        if (rc) {
            pho_error(rc, "Error when tape library lookup of drive '%s'",
                      dev_name);
            final_status = -rc;
            goto clean;
        }

        if (dev_info.ldi_full) {
            pho_error(-EINVAL,
                      "Error: drive %s is full, we only run this test on "
                      "initially empty drives", dev_name);
            final_status = EINVAL;
            goto clean;
        }

        nb_devices++;
        devices_status = xrealloc(devices_status,
                                  sizeof(*devices_status) * nb_devices);

        devices_status[nb_devices - 1].dev_name = dev_name;
        devices_status[nb_devices - 1].dev_addr = dev_info.ldi_addr;
        devices_status[nb_devices - 1].failed = false;
        devices_status[nb_devices - 1].tape_to_load_unload = NULL;
    }

    pho_info("We lookup from the tape library %u devices address from the "
             "command line.", nb_devices);

    /* tapes_by_name from conf */
    for (tape_name = strtok_r(argv[2], ",", &saveptr);
         tape_name != NULL;
         tape_name = strtok_r(NULL, ",", &saveptr)) {
        nb_tapes++;
        tapes_name = xrealloc(tapes_name, sizeof(*tapes_name) * nb_tapes);
        tapes_name[nb_tapes - 1] = tape_name;
    }

    pho_info("We got %u tape names to load/unload from the command line.",
             nb_tapes);

    for (tape_index = 0; tape_index < nb_tapes; tape_index++) {
find_a_device:
        for (dev_index = 0; dev_index < nb_devices; dev_index++) {
            if (devices_status[dev_index].failed)
                goto final_check;

            if (devices_status[dev_index].tape_to_load_unload == NULL) {
                devices_status[dev_index].tape_to_load_unload =
                    tapes_name[tape_index];
                rc = pthread_create(&devices_status[dev_index].thread_id, NULL,
                                    dev_load_unload,
                                    &devices_status[dev_index]);
                if (rc) {
                    pho_error(-rc,
                              "Error when creating dev_load_unload thread on "
                              "device %s for tape '%s'",
                              devices_status[dev_index].dev_name,
                              tapes_name[tape_index]);
                    devices_status[dev_index].failed = true;
                    goto final_check;
                }

                break;
            }
        }

        /* no device is available, we wait before trying again */
        if (dev_index == nb_devices) {
            rc = usleep(NB_USECOND_SLEEP);
            if (rc) {
                pho_error(-rc, "Error when waiting %u useconds",
                          NB_USECOND_SLEEP);
                final_status = rc;
                break;
            }

            goto find_a_device;
        }
    }

final_check:
    for (dev_index = 0; dev_index < nb_devices; dev_index++) {
        if (devices_status[dev_index].tape_to_load_unload != NULL &&
            !devices_status[dev_index].failed) {
            rc = pthread_join(devices_status[dev_index].thread_id, NULL);
            if (rc) {
                pho_error(-rc, "Error when joining thread of device '%s'",
                          devices_status[dev_index].dev_name);
                devices_status[dev_index].failed = true;
            }
        }

        if (devices_status[dev_index].failed)
            final_status = EXIT_FAILURE;
    }

clean:
    free(tapes_name);
    free(devices_status);
    rc = ldm_lib_close(&lib_hdl);
    if (rc) {
        pho_error(rc, "Error when closing tape library handle");
        final_status = -rc;
    }

    exit(final_status);
}
