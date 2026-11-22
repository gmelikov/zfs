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
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "global"

log_assert "Relocated file blocks survive removal of their special vdev"

typeset work=$(mktemp -d)
typeset ds=$TESTPOOL/$TESTFS
typeset mnt=$work/fs
typeset result cursor count=0
typeset -i steps=0

function cleanup
{
	if ! poolexists "$TESTPOOL"; then
		zpool import -N -d "$work" -d "$work/live" "$TESTPOOL" \
		    >/dev/null 2>&1
	fi
	if poolexists "$TESTPOOL"; then
		destroy_pool "$TESTPOOL"
	fi
	log_must rm -rf "$work"
}
log_onexit cleanup

log_must mkdir "$work/live"
log_must mkfile -n "$MINVDEVSIZE" "$work/live/normal"
log_must mkfile -n "$MINVDEVSIZE" "$work/live/normal2"
log_must mkfile -n "$MINVDEVSIZE" "$work/special"
log_must zpool create -o cachefile=none -o ashift=12 -O mountpoint=none \
    "$TESTPOOL" "$work/live/normal" "$work/live/normal2" \
    special "$work/special"
log_must test "$(zpool get -H -o value feature@brt_relocation "$TESTPOOL")" \
    = disabled
log_must zpool set feature@brt_relocation=enabled "$TESTPOOL"
log_must zfs create -o mountpoint="$mnt" -o recordsize=16K \
    -o compression=off -o special_small_blocks=16K "$ds"
log_must dd if=/dev/urandom of="$work/expected" bs=16k count=8
log_must cp "$work/expected" "$mnt/source"
sync_pool "$TESTPOOL"
log_must clonefile -f "$mnt/source" "$mnt/clone"
sync_pool "$TESTPOOL"
[[ $(zpool get -Hp -o value bclonesaved "$TESTPOOL") -eq 131072 ]] ||
    log_fail "Fixture failed to clone the data blocks"
log_must zfs snapshot "$ds@hold"

result=$(zpool relocate -b 64K "$TESTPOOL" "$work/special") ||
    log_fail "Initial relocation failed: $result"
[[ $result == *done=0* ]] ||
    log_fail "Unexpected initial relocation result: $result"
cursor=${result##*cursor=}
typeset first_blocks=${result%% *}
count=${first_blocks#blocks=}
(( count > 0 )) || log_fail "Initial batch moved no blocks"
log_must zpool export "$TESTPOOL"
log_must zpool import -d "$work" -d "$work/live" "$TESTPOOL"
sync_pool "$TESTPOOL"
log_must cmp "$work/expected" "$mnt/source"
log_must cmp "$work/expected" "$mnt/clone"
while [[ $result != *done=1* ]]; do
	(( ++steps <= 32 )) || log_fail "Relocation failed to finish"
	result=$(zpool relocate -b 64K -c "$cursor" \
	    "$TESTPOOL" "$work/special") || log_fail "$result"
	typeset blocks=${result%% *}
	(( count += ${blocks#blocks=} ))
	cursor=${result##*cursor=}
done
(( count >= 8 )) || log_fail "Expected at least 8 blocks, moved $count"

log_must zpool export "$TESTPOOL"
log_must eval "zdb -e -p '$work' -y '$TESTPOOL' > '$work/livelist'"
log_must eval "zdb -e -p '$work' -p '$work/live' -TTT '$TESTPOOL' > '$work/histo'"
log_must eval "zdb -e -p '$work' -p '$work/live' -TTTT '$TESTPOOL' > '$work/brt'"
typeset relocated_histogram=$(awk '
/BRT: vdev 2: DVAs with 2\^n refcnts:/ { in_histo = 1; next }
in_histo && /^[[:space:]]*[0-9]+:/ { total += $2 }
END { print total }
' "$work/histo")
(( relocated_histogram > 0 )) ||
    log_fail "zdb histogram omitted relocated clone references"
typeset destination=$(awk '/ -> / { split($3, dva, ":"); print dva[1]; exit }' \
    "$work/brt")
case "$destination" in
0) destination=$work/live/normal ;;
1) destination=$work/live/normal2 ;;
*) log_fail "Unexpected relocation destination: $destination" ;;
esac
log_must zpool import -d "$work" -d "$work/live" "$TESTPOOL"
log_must zpool remove -w "$TESTPOOL" "$destination"
log_must cmp "$work/expected" "$mnt/source"
log_must cmp "$work/expected" "$mnt/clone"

log_must zpool remove -w "$TESTPOOL" "$work/special"
log_must zpool export "$TESTPOOL"
log_must mv "$work/special" "$work/removed"
log_must zpool import -d "$work/live" "$TESTPOOL"
log_must cmp "$work/expected" "$mnt/source"
log_must cmp "$work/expected" "$mnt/clone"
log_must cmp "$work/expected" "$mnt/.zfs/snapshot/hold/source"
log_must zpool scrub -w "$TESTPOOL"
log_must check_pool_status "$TESTPOOL" "errors" "No known data errors"
log_must zpool checkpoint "$TESTPOOL"
log_must zfs destroy "$ds@hold"
log_must rm "$mnt/source" "$mnt/clone"
sync_pool "$TESTPOOL"
log_must zpool wait -t free "$TESTPOOL"
sync_pool "$TESTPOOL"
log_must zpool export "$TESTPOOL"
log_must zpool import --rewind-to-checkpoint -d "$work/live" "$TESTPOOL"
log_must cmp "$work/expected" "$mnt/source"
log_must cmp "$work/expected" "$mnt/clone"
log_must cmp "$work/expected" "$mnt/.zfs/snapshot/hold/source"
log_must zfs destroy "$ds@hold"
log_must rm "$mnt/source" "$mnt/clone"
log_must zfs destroy "$ds"
sync_pool "$TESTPOOL"
log_must zpool wait -t free "$TESTPOOL"
sync_pool "$TESTPOOL"
log_must zpool export "$TESTPOOL"
log_must zdb -e -p "$work/live" -bcc "$TESTPOOL"

log_pass "Relocated blocks survived removal, import and their final frees"
