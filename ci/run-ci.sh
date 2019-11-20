#!/bin/sh

# -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
# vim:expandtab:shiftwidth=4:tabstop=4:

# This scripts configure/compile/run phobos tests.

# (c) 2014-2019 CEA/DAM
# Licensed under the terms of the GNU Lesser GPL License version 2.1

set -xe

#set phobos root as cwd from phobos/ci directory
cur_dir=$(dirname $(readlink -m $0))
cd "$cur_dir"/..

# export PKG_CONFIG_PATH=/usr/pgsql-9.4/lib/pkgconfig;
./autogen.sh
./configure

if [ "$1" != "check-valgrind" ]; then
    make rpm
    make clean || cat src/tests/test-suite.log
fi

make
# FIXME: when cloning the repo, some scripts do not have o+rx
# permissions, it is however necessary to execute them as postgres
chmod o+rx . .. ./scripts/phobos_db{,_local}
# Need to give rx permissions to the following files to let libtool use them
# when running valgrind tests
chmod o+rx ./scripts/pho_ldm_helper ./src/cli/scripts/phobos
sudo -u postgres ./scripts/phobos_db_local drop_db || true
sudo -u postgres ./scripts/phobos_db_local setup_db -s -p phobos
export VERBOSE=1
if [ "$1" = "check-valgrind" ]; then
    sudo -E make check-valgrind
else
    sudo -E make check
fi
