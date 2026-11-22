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
log_assert "Relocated dedup blocks survive extension and DDT repair"

typeset work=$(mktemp -d)
typeset ds=$TESTPOOL/$TESTFS
typeset ds2=$TESTPOOL/second
typeset mnt=$work/fs
typeset mnt2=$work/fs2
typeset source object cursor result target
typeset -i steps
typeset reserve=$(get_tunable SPECIAL_CLASS_METADATA_RESERVE_PCT)

function cleanup
{
	set_tunable32 SPECIAL_CLASS_METADATA_RESERVE_PCT "$reserve"
	if ! poolexists "$TESTPOOL"; then
		zpool import -N -d "$work" "$TESTPOOL" >/dev/null 2>&1
	fi
	if poolexists "$TESTPOOL"; then
		destroy_pool "$TESTPOOL"
	fi
	log_must rm -rf "$work"
}
log_onexit cleanup

function create_fixture
{
	typeset feature=$1
	typeset special_size=${2:-$MINVDEVSIZE}
	log_must rm -f "$work/normal" "$work/special"
	log_must mkfile -n "$MINVDEVSIZE" "$work/normal"
	log_must mkfile -n "$special_size" "$work/special"
	log_must zpool create -f -o "feature@fast_dedup=$feature" \
	    -o feature@brt_relocation=enabled -o cachefile=none -o ashift=12 \
	    -O mountpoint=none "$TESTPOOL" "$work/normal" special "$work/special"
	log_must zfs create -o mountpoint="$mnt" -o primarycache=none \
	    -o dedup=on -o compression=off -o recordsize=16K \
	    -o special_small_blocks=16K "$ds"
	log_must cp "$work/expected" "$mnt/a"
	log_must cp "$work/expected" "$mnt/b"
	sync_pool "$TESTPOOL"
	object=$(get_objnum "$mnt/a")
	log_must zpool export "$TESTPOOL"
	log_must eval "zdb -e -p '$work' -dddddd '$ds' '$object' > '$work/bp'"
	source=$(awk '$2 == "L0" && $3 ~ /^1:/ {
	    split($3, d, ":"); print d[2]; exit }' "$work/bp")
	[[ -n $source ]] || log_fail "Dedup block is not on special vdev"
	log_must zpool import -d "$work" "$TESTPOOL"
}

function relocate
{
	steps=0
	result=$(zpool relocate -b 16M "$TESTPOOL" "$work/special") ||
	    log_fail "Relocation failed: $result"
	while [[ $result != *done=1* ]]; do
		(( ++steps <= 64 )) || log_fail "Relocation failed to finish"
		cursor=${result##*cursor=}
		result=$(zpool relocate -b 16M -c "$cursor" "$TESTPOOL" \
		    "$work/special") || log_fail "Relocation failed: $result"
	done
	log_must zpool export "$TESTPOOL"
	log_must eval "zdb -e -p '$work' -TTTT '$TESTPOOL' > '$work/brt'"
	target=$(awk -v key="1:$source@" 'index($1, key) == 1 && $2 == "->" {
	    split($3, d, ":"); split(d[2], b, "@"); print b[1]; exit }' \
	    "$work/brt")
	[[ -n $target ]] || log_fail "Dedup block was not relocated"
}

log_must dd if=/dev/urandom of="$work/expected" bs=16k count=1

# A flat DDT entry can acquire a second DVA after the move.
create_fixture enabled
log_must zfs snapshot "$ds@hold"
relocate
log_must zpool import -d "$work" "$TESTPOOL"
log_must zfs create -o mountpoint="$mnt2" -o primarycache=none \
    -o dedup=on -o copies=2 -o compression=off -o recordsize=16K \
    -o special_small_blocks=16K "$ds2"
log_must cp "$work/expected" "$mnt2/two"
sync_pool "$TESTPOOL"
object=$(get_objnum "$mnt2/two")
log_must zpool export "$TESTPOOL"
log_must eval "zdb -e -p '$work' -dddddd '$ds2' '$object' > '$work/extended'"
awk -v source="$source" '$2 == "L0" {
    split($3, d, ":"); if (d[1] == 1 && d[2] == source &&
    $4 ~ /^[0-9]+:/) found = 1 } END { exit !found }' "$work/extended" ||
    log_fail "The flat DDT entry did not acquire a second DVA"
log_must zpool import -d "$work" "$TESTPOOL"
log_must cmp "$work/expected" "$mnt2/two"
log_must clonefile -f "$mnt/a" "$mnt/clone"
sync_pool "$TESTPOOL"
log_must cmp "$work/expected" "$mnt/clone"
log_must zpool scrub -w "$TESTPOOL"
log_must zfs destroy "$ds@hold"
log_must zfs destroy "$ds2"
log_must rm "$mnt/a" "$mnt/b" "$mnt/clone"
sync_pool "$TESTPOOL"
log_must zpool wait -t free "$TESTPOOL"
sync_pool "$TESTPOOL"
log_must zpool export "$TESTPOOL"
log_must zdb -e -p "$work" -bcc "$TESTPOOL"
log_must zpool import -d "$work" "$TESTPOOL"
log_must destroy_pool "$TESTPOOL"

# Traditional DDT repair must write the destination, not the freed source.
# A small special vdev forces the fill writes to revisit freed space.
log_must set_tunable32 SPECIAL_CLASS_METADATA_RESERVE_PCT 0
log_must dd if=/dev/urandom of="$work/expected" bs=16k count=96
create_fixture disabled 64M
log_must zfs create -o mountpoint="$mnt2" -o primarycache=none \
    -o dedup=on -o copies=2 -o compression=off -o recordsize=16K \
    -o special_small_blocks=16K "$ds2"
log_must cp "$work/expected" "$mnt2/two"
sync_pool "$TESTPOOL"
relocate
log_must zpool import -d "$work" "$TESTPOOL"
typeset fill fillds
typeset -i attempt
typeset match=""
for ((attempt = 0; attempt < 9; attempt++)); do
	fill=$work/fill$attempt
	fillds=$TESTPOOL/fill$attempt
	log_must zfs create -o mountpoint="$fill" -o dedup=off \
	    -o compression=off -o recordsize=16K -o special_small_blocks=16K \
	    "$fillds"
	log_must dd if=/dev/urandom of="$fill/data" bs=1048576 count=32
	sync_pool "$TESTPOOL"
	sync_pool "$TESTPOOL"
	typeset fill_sum=$(cksum "$fill/data" | awk '{print $1, $2}')
	object=$(get_objnum "$fill/data")
	log_must zpool export "$TESTPOOL"
	log_must eval "zdb -e -p '$work' -dddddd '$fillds' '$object' > '$work/fill-bp'"
	match=$(awk '
	    function hex(s,    i, n) {
		for (i = 1; i <= length(s); i++)
		    n = n * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
		return n
	    }
	    FILENAME == ARGV[1] && $2 == "->" {
		split($1, key, "@")
		relocated[key[1]] = 1
		next
	    }
	    FILENAME == ARGV[2] && $2 == "L0" {
		split($3, d, ":")
		if (d[1] == 1 && (d[1] ":" d[2]) in relocated) {
		    old[hex(d[2])] = $1
		    address[hex(d[2])] = d[2]
		}
		next
	    }
	    FILENAME == ARGV[3] && $2 == "L0" {
		split($3, d, ":")
		if (d[1] != 1) next
		start = hex(d[2])
		end = start + hex(d[3])
		for (sector = start; sector < end; sector += 4096)
		    if (sector in old) { print old[sector], address[sector]; exit }
	    }
	    ' "$work/brt" "$work/bp" "$work/fill-bp")
	if [[ -n $match ]]; then
		break
	fi
	(( attempt < 8 )) || log_fail "No relocated source address was reused"
	log_must zpool import -d "$work" "$TESTPOOL"
done
set -A reused $match
typeset -i file_sector=$((16#${reused[0]} / 512))
source=${reused[1]}
target=$(awk -v key="1:$source@" 'index($1, key) == 1 && $2 == "->" {
    split($3, d, ":"); split(d[2], b, "@"); print b[1]; exit }' "$work/brt")
[[ -n $target ]] || log_fail "Reused source has no relocation mapping"
typeset -i source_sector=$(( (16#$source + 4 * 1024 * 1024) / 512 ))
typeset -i target_sector=$(( (16#$target + 4 * 1024 * 1024) / 512 ))
log_must dd if="$work/special" of="$work/sentinel" bs=512 \
    skip="$source_sector" count=1
log_must dd if=/dev/urandom of="$work/normal" bs=512 \
    seek="$target_sector" count=1 conv=notrunc
log_must zpool import -d "$work" "$TESTPOOL"
log_must cmp "$work/expected" "$mnt/a"
sync_pool "$TESTPOOL"
sync_pool "$TESTPOOL"
[[ $(cksum "$fill/data" | awk '{print $1, $2}') == "$fill_sum" ]] ||
    log_fail "DDT repair damaged the file reusing the source address"
log_must zpool export "$TESTPOOL"
log_must dd if="$work/special" of="$work/source-after" bs=512 \
    skip="$source_sector" count=1
log_must cmp "$work/sentinel" "$work/source-after"
log_must dd if="$work/normal" of="$work/target-after" bs=512 \
    skip="$target_sector" count=1
log_must dd if="$work/expected" of="$work/target-expected" bs=512 \
    skip="$file_sector" count=1
log_must cmp "$work/target-expected" "$work/target-after"
log_must zdb -e -p "$work" -bcc "$TESTPOOL"

log_pass "Dedup extension and repair preserved relocated blocks"
