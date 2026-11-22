#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"
log_assert "Relocation skips incompatible sizes and continues the cursor"

typeset work=$(mktemp -d)
typeset ds=$TESTPOOL/data
typeset result

function cleanup
{
	if poolexists "$TESTPOOL"; then
		destroy_pool "$TESTPOOL"
	fi
	log_must rm -rf "$work"
}
log_onexit cleanup

for disk in normal0 normal1 normal2 special0 special1; do
	log_must mkfile -n "$MINVDEVSIZE" "$work/$disk"
done
log_must zpool create -o cachefile=none -o ashift=12 -O mountpoint=none \
	"$TESTPOOL" raidz1 "$work/normal0" "$work/normal1" \
	"$work/normal2" special mirror "$work/special0" "$work/special1"
log_must zpool set feature@brt_relocation=enabled "$TESTPOOL"
log_must mkdir "$work/mnt"
log_must zfs create -o mountpoint="$work/mnt" -o recordsize=16K \
	-o compression=off -o special_small_blocks=16K "$ds"
log_must dd if=/dev/urandom of="$work/expected" bs=16K count=32
log_must cp "$work/expected" "$work/mnt/data"
sync_pool "$TESTPOOL"

result=$(zpool relocate -b 16M "$TESTPOOL" mirror-1) ||
	log_fail "Relocation failed: $result"
[[ $result == *done=1* ]] || log_fail "Cursor did not finish: $result"
[[ $result == *skipped=*[1-9]* ]] ||
	log_fail "Expected incompatible blocks to be skipped: $result"
log_must cmp "$work/expected" "$work/mnt/data"

log_pass "Relocation skipped incompatible sizes and reached the end"
