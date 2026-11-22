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
log_assert "Relocated volume blocks survive import and snapshot deletion"

typeset work=$(mktemp -d)
typeset vol=$TESTPOOL/$TESTVOL
typeset clone=$TESTPOOL/clonevol
typeset result cursor
typeset writer
typeset -i count=0 steps=0

function export_pool_wait
{
	typeset output
	typeset -i attempt
	for (( attempt = 0; attempt < 10; attempt++ )); do
		if output=$(zpool export "$TESTPOOL" 2>&1); then
			return 0
		fi
		[[ $output == *"pool is busy"* ]] || {
			print -u2 -- "$output"
			return 1
		}
		sleep 1
	done
	print -u2 -- "$output"
	return 1
}

function cleanup
{
	if [[ -n $writer ]]; then
		touch "$work/stop"
		wait "$writer"
	fi
	if ! poolexists "$TESTPOOL"; then
		zpool import -N -d "$work" "$TESTPOOL" >/dev/null 2>&1
	fi
	if poolexists "$TESTPOOL"; then
		destroy_pool "$TESTPOOL"
	fi
	log_must rm -rf "$work"
}
log_onexit cleanup

log_must mkfile -n "$MINVDEVSIZE" "$work/normal"
log_must mkfile -n "$MINVDEVSIZE" "$work/special"
log_must zpool create -o cachefile=none -o ashift=12 -O mountpoint=none \
    "$TESTPOOL" "$work/normal" special "$work/special"
log_must zpool set feature@brt_relocation=enabled "$TESTPOOL"
log_must zfs create -V 16M -b 16K -o compression=off -o copies=3 \
    -o special_small_blocks=16K "$vol"
block_device_wait "$ZVOL_DEVDIR/$vol"
log_must dd if=/dev/urandom of="$work/expected" bs=16k count=8
log_must dd if="$work/expected" of="$ZVOL_DEVDIR/$vol" bs=16k
sync_pool "$TESTPOOL"
log_must zfs snapshot "$vol@hold"
log_must zfs clone "$vol@hold" "$clone"
block_device_wait "$ZVOL_DEVDIR/$clone"
log_must dd if=/dev/urandom of="$work/updated" bs=16k count=8
(
	while [[ ! -f "$work/stop" ]]; do
		dd if="$work/updated" of="$ZVOL_DEVDIR/$vol" bs=16k \
		    >/dev/null 2>&1 || exit 1
		touch "$work/writing"
	done
) &
writer=$!
for (( i = 0; i < 10; i++ )); do
	[[ -f "$work/writing" ]] && break
	sleep 1
done
[[ -f "$work/writing" ]] || log_fail "Volume writer did not start"

result=$(zpool relocate -b 2M "$TESTPOOL" "$work/special") ||
    log_fail "Relocation failed: $result"
while :; do
	typeset blocks=${result%% *}
	(( count += ${blocks#blocks=} ))
	[[ $result == *done=1* ]] && break
	(( ++steps <= 64 )) || log_fail "Relocation failed to finish"
	cursor=${result##*cursor=}
	result=$(zpool relocate -b 2M -c "$cursor" "$TESTPOOL" \
	    "$work/special") || log_fail "Relocation failed: $result"
done
(( count >= 8 )) || log_fail "Expected at least 8 blocks, moved $count"
log_must kill -0 "$writer"
log_must touch "$work/stop"
log_must wait "$writer"
writer=""
log_must dd if="$work/updated" of="$ZVOL_DEVDIR/$vol" bs=16k
sync_pool "$TESTPOOL"

export_pool_wait || log_fail "Pool stayed busy after closing the writer"
log_must eval "zdb -e -p '$work' -TTTT '$TESTPOOL' > '$work/brt'"
[[ $(grep -c ' copies=3$' "$work/brt") -ge 8 ]] ||
    log_fail "Expected three destination copies for the volume blocks"
typeset address=$(awk '/ copies=3$/ { print $3; exit }' "$work/brt")
address=${address#*:}
address=${address%@*}
# File vdev data begins after the 4 MiB label and boot area.
typeset -i sector=$(( (16#$address + 4 * 1024 * 1024) / 512 ))
log_must dd if="$work/normal" of="$work/sector" bs=512 skip="$sector" \
    count=1
log_must dd if=/dev/urandom of="$work/normal" bs=512 seek="$sector" \
    count=1 conv=notrunc
log_must dd if="$work/normal" of="$work/corrupt" bs=512 skip="$sector" \
    count=1
log_mustnot cmp "$work/sector" "$work/corrupt"
log_must zpool import -d "$work" "$TESTPOOL"
block_device_wait "$ZVOL_DEVDIR/$vol"
log_must dd if="$ZVOL_DEVDIR/$vol" of="$work/actual" bs=16k count=8
log_must cmp "$work/updated" "$work/actual"
block_device_wait "$ZVOL_DEVDIR/$clone"
log_must dd if="$ZVOL_DEVDIR/$clone" of="$work/actual" bs=16k count=8
log_must cmp "$work/expected" "$work/actual"
log_must zpool scrub -w "$TESTPOOL"
log_must check_pool_status "$TESTPOOL" "errors" "No known data errors"
log_must zpool export "$TESTPOOL"
log_must dd if="$work/normal" of="$work/repaired" bs=512 skip="$sector" \
    count=1
log_must cmp "$work/sector" "$work/repaired"
log_must zpool import -d "$work" "$TESTPOOL"
log_must zfs destroy "$clone"
log_must zfs destroy "$vol@hold"
log_must zfs destroy "$vol"
sync_pool "$TESTPOOL"
log_must zpool wait -t free "$TESTPOOL"
sync_pool "$TESTPOOL"
log_must zpool export "$TESTPOOL"
log_must zdb -e -p "$work" -bcc "$TESTPOOL"

log_pass "Relocated volume blocks survived import and their final frees"
