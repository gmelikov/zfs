#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source. A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"
log_assert "Relocated metadata survives import and snapshot deletion"

typeset work=$(mktemp -d)
typeset ds=$TESTPOOL/$TESTFS
typeset mnt=$work/fs
typeset result cursor
typeset -i count=0 steps=0

function cleanup
{
	if ! poolexists "$TESTPOOL"; then
		zpool import -N -d "$work" "$TESTPOOL" >/dev/null 2>&1
	fi
	if poolexists "$TESTPOOL"; then
		destroy_pool "$TESTPOOL"
	fi
	log_must rm -rf "$work"
}
log_onexit cleanup

log_must mkfile -n "$MINVDEVSIZE" "$work/normal0"
log_must mkfile -n "$MINVDEVSIZE" "$work/normal1"
log_must mkfile -n "$MINVDEVSIZE" "$work/special0"
log_must mkfile -n "$MINVDEVSIZE" "$work/special1"
log_must zpool create -o cachefile=none -o ashift=12 -O mountpoint=none \
    "$TESTPOOL" mirror "$work/normal0" "$work/normal1" \
    special mirror "$work/special0" "$work/special1"
log_must zpool set feature@brt_relocation=enabled "$TESTPOOL"
log_must zfs create -o mountpoint="$mnt" -o compression=off \
    -o recordsize=16K -o primarycache=none -o special_small_blocks=0 "$ds"
for (( i = 1; i <= 200; i++ )); do
	log_must mkdir "$mnt/dir$i"
done
log_must dd if=/dev/urandom of="$work/expected" bs=16k count=256
log_must cp "$work/expected" "$mnt/dir1/data"
sync_pool "$TESTPOOL"
log_must zfs snapshot "$ds@hold"
log_must mv "$mnt/dir1" "$mnt/renamed"
sync_pool "$TESTPOOL"
typeset object=$(get_objnum "$mnt/renamed/data")
log_must zpool export "$TESTPOOL"
log_must eval "zdb -e -p '$work' -ddddd '$ds' '$object' > '$work/blocks'"
awk '$2 == "L1" && $3 ~ /^1:/ { split($3, d, ":");
    print d[1] ":" d[2] "@" }' "$work/blocks" > "$work/indirect"
[[ -s "$work/indirect" ]] || log_fail "No indirect block on special vdev"
log_must zpool import -d "$work" "$TESTPOOL"
log_must zpool offline "$TESTPOOL" "$work/normal1"

result=$(zpool relocate -b 16M "$TESTPOOL" mirror-1) ||
    log_fail "Relocation failed: $result"
while :; do
	typeset blocks=${result%% *}
	(( count += ${blocks#blocks=} ))
	[[ $result == *done=1* ]] && break
	(( ++steps <= 64 )) || log_fail "Relocation failed to finish"
	cursor=${result##*cursor=}
	result=$(zpool relocate -b 16M -c "$cursor" "$TESTPOOL" \
	    mirror-1) || log_fail "Relocation failed: $result"
done
(( count > 0 )) || log_fail "No metadata blocks were relocated"

log_must zpool online "$TESTPOOL" "$work/normal1"
log_must wait_resilvered "$TESTPOOL" 60
log_must is_pool_resilvered "$TESTPOOL"
log_must zpool wait -t resilver "$TESTPOOL"
log_must zpool offline "$TESTPOOL" "$work/normal0"
log_must zpool export "$TESTPOOL"
log_must eval "zdb -e -p '$work' -TTTT '$TESTPOOL' > '$work/brt'"
while read address; do
	grep -F "$address" "$work/brt" >/dev/null ||
	    log_fail "Indirect block $address was not relocated"
done < "$work/indirect"
log_must zpool import -d "$work" "$TESTPOOL"
log_must cmp "$work/expected" "$mnt/renamed/data"
log_must cmp "$work/expected" "$mnt/.zfs/snapshot/hold/dir1/data"
log_must test -d "$mnt/dir200"
log_must zpool online "$TESTPOOL" "$work/normal0"
log_must zpool wait -t resilver "$TESTPOOL"
log_must zpool scrub -w "$TESTPOOL"
log_must check_pool_status "$TESTPOOL" "errors" "No known data errors"
log_must zfs destroy "$ds@hold"
log_must zfs destroy "$ds"
sync_pool "$TESTPOOL"
log_must zpool wait -t free "$TESTPOOL"
sync_pool "$TESTPOOL"
log_must zpool export "$TESTPOOL"
log_must zdb -e -p "$work" -bcc "$TESTPOOL"

log_pass "Relocated metadata survived import and its final frees"
