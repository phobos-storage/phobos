#!/usr/bin/env python3

#
#  All rights reserved (c) 2014-2022 CEA/DAM.
#
#  This file is part of Phobos.
#
#  Phobos is free software: you can redistribute it and/or modify it under
#  the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation, either version 2.1 of the License, or
#  (at your option) any later version.
#
#  Phobos is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
#

"""
Provide access to LDM functionality with the right level (tm) of abstraction.
"""

import json

from ctypes import byref, c_char_p, c_int, c_void_p, POINTER, Structure

from phobos.core.ffi import LIBPHOBOS, pho_rc_check, pho_rc_func
from phobos.core.glue import jansson_dumps # pylint: disable=no-name-in-module

class DevState(Structure): # pylint: disable=too-few-public-methods
    """Device information as managed by LDM."""
    _fields_ = [
        ('lds_family', c_int),
        ('lds_model', c_char_p),
        ('lds_serial', c_char_p),
    ]

    def __del__(self):
        """Free allocated memory on garbage collection"""
        LIBPHOBOS.ldm_dev_state_fini(byref(self))

class DevAdapter(Structure): # pylint: disable=too-few-public-methods
    """Opaque device handle."""
    _fields_ = [
        ('dev_lookup', c_void_p),
        ('dev_query', c_void_p),
        ('dev_load', c_void_p),
        ('dev_eject', c_void_p)
    ]

class LibHandle(Structure): # pylint: disable=too-few-public-methods
    """Opaque lib handle"""
    _fields_ = [('_lh_lib', c_void_p)]


class LibAdapter(Structure): # pylint: disable=too-few-public-methods
    """Opaque lib adapter."""
    _fields_ = [
        ('_lib_open', pho_rc_func("lib_open", c_void_p, c_char_p)),
        ('_lib_close', pho_rc_func("lib_close", c_void_p)),
        ('_lib_drive_lookup',
         pho_rc_func("lib_drive_lookup", c_void_p, c_char_p, c_void_p)),
        ('_lib_media_lookup',
         pho_rc_func("lib_media_lookup", c_void_p, c_char_p, c_void_p)),
        ('_lib_media_move',
         pho_rc_func("lib_media_move", c_void_p, c_void_p, c_void_p)),
        ('_lib_scan', pho_rc_func("lib_scan", c_void_p, POINTER(c_void_p))),
        ('_lib_hdl', LibHandle),
    ]

    def __init__(self, lib_type, lib_dev_path):
        super().__init__()
        LIBPHOBOS.get_lib_adapter.errcheck = pho_rc_check
        LIBPHOBOS.get_lib_adapter(lib_type, byref(self))
        if self._lib_open is not None:
            self._lib_open(byref(self._lib_hdl), lib_dev_path.encode('utf-8'))

    def __del__(self):
        # pylint: disable=protected-access
        if self._lib_hdl._lh_lib is not None and self._lib_close is not None:
            self._lib_close(byref(self._lib_hdl))

    def scan(self):
        """Scan and return a list of dictionnaries representing the properties
        of elements in a library of type lib_type.

        The only working implementation is for PHO_LIB_SCSI, which performs a
        SCSI scan of a given device.
        """
        jansson_t = c_void_p(None)
        if self._lib_scan is None:
            return {}
        self._lib_scan(byref(self._lib_hdl), byref(jansson_t))
        return json.loads(jansson_dumps(jansson_t.value))
