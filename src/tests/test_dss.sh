#!/bin/bash
set -e

test_bin_dir=$(dirname "${BASH_SOURCE[0]}")
test_bin="$test_bin_dir/test_dss"

. $test_bin_dir/setup_db.sh

function clean_test
{
    echo "cleaning..."
    #drop_tables
}

function error
{
    echo "ERROR: $*" >&2
    clean_test
    exit 1
}

function test_check_get
{
    local type=$1
    local crit=$2


    $test_bin get "$type" "$crit"
}

function test_check_set
{
    local type=$1
    local crit=$2
    local action=$3
    local expect_fail=$4
    local rc=0

    $test_bin set "$type" "$crit" "$action" || rc=$?

    if [ -z "$expect_fail" ]
    then
        if [ $rc -ne 0 ]
        then
            echo "$test_bin failed with $rc"
            return 1
        fi
    else
        if [ $rc -eq 0 ]
        then
            echo "$test_bin succeeded against expectations"
            return 1
        fi
    fi
    return 0
}

function test_check_lock
{
    local target=$1
    local action=$2
    local expect_fail=$3
    local rc=0

    $test_bin $action $target || rc=$?

    if [ -z "$expect_fail" ]
    then
        if [ $rc -ne 0 ]
        then
            echo "$test_bin failed with $rc"
            return 1
        fi
    else
        if [ $rc -eq 0 ]
        then
            echo "$test_bin succeeded against expectations"
            return 1
        fi
    fi
    return 0
}


trap clean_test ERR EXIT

clean_test
setup_tables
insert_examples

echo

echo "**** TESTS: DSS_GET DEV ****"
test_check_get "device" 'all'
test_check_get "device" '{"$LIKE": {"DSS:DEV::path": "/dev%"}}'
test_check_get "device" '{"DSS::DEV::family": "tape"}'
test_check_get "device" '{"DSS::DEV::family": "dir"}'
test_check_get "device" '{"$NOR": [{"DSS::DEV::id": "foo"}]}'
test_check_get "device" '{"$NOR": [{"DSS::DEV::host": "foo"}]}'
test_check_get "device" '{"DSS::DEV::adm_status": "unlocked"}'
test_check_get "device" '{"DSS::DEV::model": "ULTRIUM-TD6"}'


echo "**** TESTS: DSS_GET MEDIA ****"
test_check_get "media" "all"
test_check_get "media" '{"$LT": {"DSS::MDA::vol_used": 42469425152}}'
test_check_get "media" '{"DSS::MDA::vol_used": 42469425152}'
test_check_get "media" '{"$GT": {"DSS::MDA::vol_used": 42469425152}}'
test_check_get "media" '{"DSS::MDA::family": "tape"}'
test_check_get "media" '{"DSS::MDA::family": "dir"}'
test_check_get "media" '{"DSS::MDA::model": "LTO6"}'
test_check_get "media" '{"$NOR": [{"DSS::MDA::id": "foo"}]}'
test_check_get "media" '{"$NOR": [{"DSS::MDA::adm_status": "unlocked"}]}'
test_check_get "media" '{"$NOR": [{"DSS::MDA::fs_type": "LTFS"}]}'
test_check_get "media" '{"$NOR": [{"DSS::MDA::addr_type": "HASH1"}]}'
test_check_get "media" '{"$NOR": [{"DSS::MDA::fs_status": "blank"}]}'

echo "**** TESTS: DSS_GET OBJECT ****"
test_check_get "object" 'all'
test_check_get "object" '{"$LIKE": {"DSS::OBJ::oid": "012%"}}'
test_check_get "object" '{"$LIKE": {"DSS::OBJ::oid": "koéèê^!$£}[<>à@\\"}}'

echo "**** TESTS: DSS_GET EXTENT ****"
test_check_get "extent" 'all'
test_check_get "extent" '{"$INJSON": {"DSS::EXT::media_idx": "073221L6"}}'
test_check_get "extent" \
  '{"$INJSON": {"DSS::EXT::media_idx": "phobos1:/tmp/pho_testdir1"}}'
test_check_get "extent" '{"$INJSON": {"DSS::EXT::media_idx": "DOESNOTEXIST"}}'
test_check_get "extent" \
  '{"$INJSON": {"DSS::EXT::media_idx": "phobos1:/tmp/doesnotexist"}}'
test_check_get "extent" '{"DSS::EXT::oid": "QQQ6ASQDSQD"}'
test_check_get "extent" '{"$NOR": [{"DSS::EXT::oid": "QQQ6ASQDSQD"}]}'
test_check_get "extent" '{"$LIKE": {"DSS::EXT::oid": "Q%D"}}'
test_check_get "extent" '{"DSS::EXT::copy_num": 0}'
test_check_get "extent" '{"DSS::EXT::state": "pending"}'
test_check_get "extent" '{"DSS::EXT::lyt_type": "simple"}'

echo "**** TEST: DSS_SET DEVICE ****"
test_check_set "device" "insert"
test_check_get "device" '{"$LIKE": {"DSS::DEV::id": "%COPY%"}}'
test_check_set "device" "update"
test_check_get "device" '{"$LIKE": {"DSS::DEV::host": "%UPDATE%"}}'
test_check_set "device" "delete"
test_check_get "device" '{"$LIKE": {"DSS::DEV::id": "%COPY%"}}'
echo "**** TEST: DSS_SET MEDIA  ****"
test_check_set "media" "insert"
test_check_get "media" '{"$LIKE": {"DSS::MDA::id": "%COPY%"}}'
test_check_set "media" "update"
test_check_get "media" '{"$GT": {"DSS::MDA::nb_obj": "1002"}}'
test_check_set "media" "delete"
test_check_get "media" '{"$LIKE": {"DSS::MDA::id": "%COPY%"}}'
echo "**** TEST: DSS_SET OBJECT  ****"
test_check_set "object" "insert"
test_check_get "object" '{"$LIKE": {"DSS::OBJ::oid": "%COPY%"}}'
#test_check_set "object" "update"
test_check_set "object" "delete"
test_check_get "object" '{"$LIKE": {"DSS::OBJ::oid": "%COPY%"}}'
echo "**** TEST: DSS_SET EXTENT  ****"
test_check_set "extent" "insert"
test_check_get "extent" '{"$LIKE": {"DSS::EXT::oid": "%COPY%"}}'
test_check_set "extent" "update"
test_check_get "extent" '{"$LIKE": {"DSS::EXT::oid": "%COPY%"}}'
test_check_set "extent" "delete" "oidtest" "FAIL"
test_check_set "extent" "delete"
test_check_get "extent" '{"$LIKE": {"DSS::EXT::oid": "%COPY%"}}'

insert_examples

echo "**** TESTS: DSS_DEVICE LOCK/UNLOCK  ****"
echo "*** TEST LOCK ***"
test_check_lock "device" "lock"
echo "*** TEST DOUBLE LOCK (EEXIST expected) ***"
test_check_lock "device" "lock" "FAIL"
echo "*** TEST UNLOCK ***"
test_check_lock "device" "unlock"
echo "*** TEST RELOCK ***"
test_check_lock "device" "lock"

echo "**** TESTS: DSS_MEDIA LOCK/UNLOCK  ****"
echo "*** TEST LOCK ***"
test_check_lock "media" "lock"
echo "*** TEST DOUBLE LOCK (EEXIST expected) ***"
test_check_lock "media" "lock" "FAIL"
echo "*** TEST UNLOCK ***"
test_check_lock "media" "unlock"
echo "*** TEST DOUBLE UNLOCK (EEXIST expected) ***"
test_check_lock "media" "unlock" "FAIL"
echo "*** TEST RELOCK ***"
test_check_lock "media" "lock"

echo "*** TEST END ***"
# Uncomment if you want the db to persist after test
# trap - EXIT ERR
