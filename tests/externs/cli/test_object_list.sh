#!/bin/bash
#
#  All rights reserved (c) 2014-2022 CEA/DAM.
#
#  This file is part of Phobos.
#
#  Phobos is free software: you can redistribute it and/or modify it under
#  the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation, either version 2.1 of the Licence, or
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
#
# Integration test for deletion feature
#

test_dir=$(dirname $(readlink -e $0))
. $test_dir/../../test_env.sh
. $test_dir/../../setup_db.sh

set -xe

function setup
{
    # start with a clean and empty DB
    setup_tables

    $PSQL << EOF
insert into object (oid, user_md)
            values ('oid1', '{}'),
                   ('oid2', '{"bloot": "bloot"}'),
                   ('blob', '{"bloot": "bloot",
                              "blobby": "bloba"}'),
                   ('lorem', '{"ipsum": "dolor",
                               "blobby": "bloba"}'),
                   ('long_md', '{"abcdefghijklmnopqrstuvwxyz":
                                 "123456789123456789123456789123456789"}');
EOF
}

function cleanup
{
    drop_tables
}

function list_error
{
    echo "An error occured while listing objects: "
    echo "Matching: $1"
    echo "Returned: $2"
    echo "Expected: $3"
    exit 1
}

function content_matching
{
    contents=$1
    output_format="$2"

    for id in "${contents[@]}"
    do
        match=$(echo "$id" | cut -d';' -f1)
        exp=$(echo -e $(echo "$id" | cut -d';' -f2))
        res=$($valg_phobos object list $output_format $match)

        if [ "$res" != "$exp" ]; then
            list_error "$match" "$res" "$exp"
        fi
    done
}

function test_object_list_pattern
{
    contents=("oid1;oid1"
              "--pattern oid;oid1\noid2"
              "--pattern --metadata bloot=bloot oid;oid2"
              "--metadata bloot=bloot blob;blob"
              ";oid1\noid2\nblob\nlorem\nlong_md"
              "--pattern OID1;"
              "--pattern --metadata bloot=bloot o;oid2\nblob"
              "--pattern --metadata blobby=bloba,bloot=bloot o;blob"
              "--pattern --metadata blobby=bloba b m;blob\nlorem")

    content_matching $contents
}

function test_object_list_max_width
{
    alphabet="abcdefghijklmnopqrstuvwxyz"
    numbers="123456789123456789123456789123456789"

    contents=("long_md;{\"abcdefghijklmnopqrstuvwx...}"
              "--max-width 15 long_md;{\"abcdefghi...}"
              "--max-width 40 long_md;{\"$alphabet\": \"1234...}"
              "--max-width 7 long_md;{\"a...}"
              "--max-width 1000 long_md;{\"$alphabet\": \"$numbers\"}"
              "--no-trunc long_md;{\"$alphabet\": \"$numbers\"}"
              "--max-width 5 --no-trunc long_md;{\"$alphabet\": \"$numbers\"}"
              "--max-width 10 --no-trunc long_md;{\"$alphabet\": \"$numbers\"}")

    content_matching $contents "-o user_md"
}

trap cleanup EXIT
setup

test_object_list_pattern
test_object_list_max_width
