#
#  All rights reserved (c) 2014-2025 CEA/DAM.
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
Copy target for Phobos CLI
"""

import sys

from phobos.cli.action.create import CreateOptHandler
from phobos.cli.action.delete import DeleteOptHandler
from phobos.cli.action.list import ListOptHandler
from phobos.cli.common import BaseResourceOptHandler, env_error_format
from phobos.cli.common.args import add_put_arguments
from phobos.cli.common.utils import check_output_attributes, get_params_status
from phobos.core.ffi import CopyInfo
from phobos.core.store import UtilClient
from phobos.output import dump_object_list


class CopyCreateOptHandler(CreateOptHandler):
    """Option handler for create action of copy target"""
    descr = 'create copy of object'

    @classmethod
    def add_options(cls, parser):
        super(CopyCreateOptHandler, cls).add_options(parser)
        parser.add_argument('oid', help='targeted object')
        parser.add_argument('copy', help='copy name')
        add_put_arguments(parser)


class CopyDeleteOptHandler(DeleteOptHandler):
    """Option handler for delete action of copy target"""
    descr = 'delete copy of object'

    @classmethod
    def add_options(cls, parser):
        super(CopyDeleteOptHandler, cls).add_options(parser)
        parser.add_argument('oid', help='targeted object')
        parser.add_argument('copy', help='copy name')


class CopyListOptHandler(ListOptHandler):
    """Option handler for list action of copy target"""
    descr = 'list all copies'

    @classmethod
    def add_options(cls, parser):
        super(CopyListOptHandler, cls).add_options(parser)

        attrs = list(CopyInfo().get_display_dict().keys())
        attrs.sort()

        parser.add_argument('-o', '--output', type=lambda t: t.split(','),
                            default='copy_name',
                            help=("attributes to output, comma-separated, "
                                  "choose from {" + " ".join(attrs) + "} "
                                  "default: %(default)s"))
        parser.add_argument('-s', '--status', action='store',
                            help="filter copies according to their "
                                 "copy_status, choose one or multiple letters "
                                 "from {i r c} for respectively: incomplete, "
                                 "readable and complete")
        parser.epilog = """About the status of the copy:
        incomplete: the copy cannot be rebuilt because it lacks some of its
                    extents,
        readable:   the copy can be rebuilt, however some of its extents were
                    not found,
        complete:   the copy is complete."""


class CopyOptHandler(BaseResourceOptHandler):
    """Option handler for copy target"""
    label = 'copy'
    descr = 'manage copies of objects'
    verbs = [
        CopyCreateOptHandler,
        CopyDeleteOptHandler,
        CopyListOptHandler,
    ]

    def exec_create(self):
        """Copy creation"""
        raise NotImplementedError(
            "This command will be implemented in a future version"
        )

    def exec_delete(self):
        """Copy deletion"""
        raise NotImplementedError(
            "This command will be implemented in a future version"
        )

    def exec_list(self):
        """Copy listing"""
        attrs = list(CopyInfo().get_display_dict().keys())
        check_output_attributes(attrs, self.params.get('output'), self.logger)

        status_number = get_params_status(self.params.get('status'),
                                          self.logger)

        client = UtilClient()

        try:
            copies = client.copy_list(self.params.get('res'),
                                      status_number)

            if copies:
                dump_object_list(copies, attr=self.params.get('output'),
                                 fmt=self.params.get('format'))
            client.list_cpy_free(copies, len(copies))

        except EnvironmentError as err:
            self.logger.error(env_error_format(err))
            sys.exit(abs(err.errno))
