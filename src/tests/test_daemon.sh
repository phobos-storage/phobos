#!/bin/bash

#
#  All rights reserved (c) 2014-2020 CEA/DAM.
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

set -xe

test_dir=$(dirname $(readlink -e $0))
. $test_dir/test_env.sh
. $test_dir/setup_db.sh

function error
{
    echo "$*"
    drop_tables
    exit 1
}

function test_multiple_instances
{
    setup_tables

    export PHOBOS_LRS_lock_file="$test_bin_dir/phobosd.lock"
    pidfile="/tmp/pidfile"

    $phobosd -i &
    first_process=$!

    sleep 1

    timeout 60 $phobosd -i &
    second_process=$!

    wait $second_process && true
    rc=$?
    kill $first_process

    unset PHOBOS_LRS_lock_file

    # Second daemon error code should be -EEXIST, which is -17
    test $rc -eq $((256 - 17)) ||
        error "Second daemon instance does not get the right error code"

    drop_tables
}

drop_tables
test_multiple_instances
