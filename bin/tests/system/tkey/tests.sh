#!/bin/sh
#
# Copyright (C) Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# See the COPYRIGHT file distributed with this work for additional
# information regarding copyright ownership.

SYSTEMTESTTOP=..
. $SYSTEMTESTTOP/conf.sh

DIGOPTS="@10.53.0.1 -p 5300"

status=0

echo_i "generating new DH key"
ret=0
dhkeyname=`$KEYGEN -T KEY -a DH -b 768 -n host client` || ret=1
if [ $ret != 0 ]; then
	echo_i "failed"
	status=`expr $status + $ret`
	echo_i "exit status: $status"
	exit $status
fi
status=`expr $status + $ret`

for owner in . foo.example.
do
	echo_i "creating new key using owner name \"$owner\""
	ret=0
	keyname=`$KEYCREATE $dhkeyname $owner` || ret=1
	if [ $ret != 0 ]; then
		echo_i "failed"
		status=`expr $status + $ret`
		echo_i "exit status: $status"
		exit $status
	fi
	status=`expr $status + $ret`

	echo_i "checking the new key"
	ret=0
	$DIG $DIGOPTS txt txt.example -k $keyname > dig.out.1 || ret=1
	grep "status: NOERROR" dig.out.1 > /dev/null || ret=1
	grep "TSIG.*hmac-md5.*NOERROR" dig.out.1 > /dev/null || ret=1
	grep "Some TSIG could not be validated" dig.out.1 > /dev/null && ret=1
	if [ $ret != 0 ]; then
		echo_i "failed"
	fi
	status=`expr $status + $ret`

	echo_i "deleting new key"
	ret=0
	$KEYDELETE $keyname || ret=1
	if [ $ret != 0 ]; then
		echo_i "failed"
	fi
	status=`expr $status + $ret`

	echo_i "checking that new key has been deleted"
	ret=0
	$DIG $DIGOPTS txt txt.example -k $keyname > dig.out.2 || ret=1
	grep "status: NOERROR" dig.out.2 > /dev/null && ret=1
	grep "TSIG.*hmac-md5.*NOERROR" dig.out.2 > /dev/null && ret=1
	grep "Some TSIG could not be validated" dig.out.2 > /dev/null || ret=1
	if [ $ret != 0 ]; then
		echo_i "failed"
	fi
	status=`expr $status + $ret`
done

echo_i "creating new key using owner name bar.example."
ret=0
keyname=`$KEYCREATE $dhkeyname bar.example.` || ret=1
if [ $ret != 0 ]; then
        echo_i "failed"
	status=`expr $status + $ret`
        echo_i "exit status: $status"
        exit $status
fi
status=`expr $status + $ret`

echo_i "checking the key with 'rndc tsig-list'"
ret=0
$RNDC -c ../common/rndc.conf -s 10.53.0.1 -p 9953 tsig-list > rndc.out.1
grep "key \"bar.example.server" rndc.out.1 > /dev/null || ret=1
if [ $ret != 0 ]; then
        echo_i "failed"
fi
status=`expr $status + $ret`

echo_i "using key in a request"
ret=0
$DIG $DIGOPTS -k $keyname txt.example txt > dig.out.3 || ret=1
grep "status: NOERROR" dig.out.3 > /dev/null || ret=1
if [ $ret != 0 ]; then
        echo_i "failed"
fi
status=`expr $status + $ret`

echo_i "deleting the key with 'rndc tsig-delete'"
ret=0
$RNDC -c ../common/rndc.conf -s 10.53.0.1 -p 9953 tsig-delete bar.example.server > /dev/null || ret=1
$RNDC -c ../common/rndc.conf -s 10.53.0.1 -p 9953 tsig-list > rndc.out.2
grep "key \"bar.example.server" rndc.out.2 > /dev/null && ret=1
$DIG $DIGOPTS -k $keyname txt.example txt > dig.out.4 || ret=1
grep "TSIG could not be validated" dig.out.4 > /dev/null || ret=1
if [ $ret != 0 ]; then
        echo_i "failed"
fi
status=`expr $status + $ret`

echo_i "recreating the bar.example. key"
ret=0
keyname=`$KEYCREATE $dhkeyname bar.example.` || ret=1
if [ $ret != 0 ]; then
        echo_i "failed"
	status=`expr $status + $ret`
        echo_i "exit status: $status"
        exit $status
fi
status=`expr $status + $ret`

echo_i "checking the new key with 'rndc tsig-list'"
ret=0
$RNDC -c ../common/rndc.conf -s 10.53.0.1 -p 9953 tsig-list > rndc.out.3
grep "key \"bar.example.server" rndc.out.3 > /dev/null || ret=1
if [ $ret != 0 ]; then
        echo_i "failed"
fi
status=`expr $status + $ret`

echo_i "using the new key in a request"
ret=0
$DIG $DIGOPTS -k $keyname txt.example txt > dig.out.5 || ret=1
grep "status: NOERROR" dig.out.5 > /dev/null || ret=1
if [ $ret != 0 ]; then
        echo_i "failed"
fi
status=`expr $status + $ret`

echo_i "exit status: $status"
[ $status -eq 0 ] || exit 1
