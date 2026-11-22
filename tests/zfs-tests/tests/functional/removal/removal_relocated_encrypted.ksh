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
log_assert "Encrypted blocks can be relocated with their key unloaded"

typeset work=$(mktemp -d)
typeset ds=$TESTPOOL/$TESTFS
typeset mnt=$work/fs
typeset result cursor
typeset -i steps=0

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

log_must mkfile -n "$MINVDEVSIZE" "$work/normal"
log_must mkfile -n "$MINVDEVSIZE" "$work/special"
log_must zpool create -o cachefile=none -o ashift=12 -O mountpoint=none \
    "$TESTPOOL" "$work/normal" special "$work/special"
log_must zpool set feature@brt_relocation=enabled "$TESTPOOL"
log_must dd if=/dev/urandom of="$work/key" bs=32 count=1
log_must zfs create -o encryption=aes-256-gcm -o keyformat=raw \
    -o keylocation="file://$work/key" -o mountpoint="$mnt" \
    -o compression=lz4 -o copies=2 -o recordsize=16K \
    -o special_small_blocks=16K "$ds"
log_must dd if=/dev/urandom of="$work/pattern" bs=1k count=1
for (( i = 0; i < 16; i++ )); do
	cat "$work/pattern" >> "$work/record"
done
for (( i = 0; i < 256; i++ )); do
	cat "$work/record" >> "$work/expected"
done
log_must cp "$work/expected" "$mnt/source"
typeset object=$(get_objnum "$mnt/source")
sync_pool "$TESTPOOL"
log_must clonefile -f "$mnt/source" "$mnt/clone"
sync_pool "$TESTPOOL"
[[ $(zpool get -Hp -o value bclonesaved "$TESTPOOL") -gt 0 ]] ||
    log_fail "Fixture failed to clone the encrypted data"
log_must zfs snapshot "$ds@hold"
sync_pool "$TESTPOOL"
log_must zfs unmount "$ds"
log_must zfs unload-key "$ds"
log_must eval "zfs send -w '$ds@hold' > '$work/before.stream'"
log_must zpool export "$TESTPOOL"
log_must eval "zdb -e -p '$work' -ddddd '$ds' '$object' > '$work/blocks'"
log_must eval "zdb -e -p '$work' -ddddd '$ds' 0 > '$work/dnodes'"
typeset root=$(sed -n \
    's/.*rootbp DVA\[0\]=<\([^:]*\):\([^:]*\):.*/\1:\2@/p' "$work/blocks")
[[ $root == 1:* ]] || log_fail "Root block is not on the special vdev"
awk '$2 == "L1" && $3 ~ /^1:/ { split($3, d, ":");
    print d[1] ":" d[2] "@" }' "$work/blocks" > "$work/indirect"
[[ -s "$work/indirect" ]] || log_fail "No indirect block on special vdev"
awk '$2 == "L0" && $3 ~ /^1:/ { split($3, d, ":");
    print d[1] ":" d[2] "@" }' "$work/dnodes" > "$work/dnode-addrs"
[[ -s "$work/dnode-addrs" ]] || log_fail "No dnode on special vdev"
awk '$2 == "L0" && $3 ~ /^1:/ { split($3, d, ":");
    print d[1] ":" d[2] }' "$work/blocks" > "$work/data-addrs"
[[ -s "$work/data-addrs" ]] || log_fail "No encrypted data on special vdev"
log_must zpool import -d "$work" "$TESTPOOL"

result=$(zpool relocate -b 2M "$TESTPOOL" "$work/special") ||
    log_fail "Relocation failed: $result"
while :; do
	[[ $result == *done=1* ]] && break
	(( ++steps <= 64 )) || log_fail "Relocation failed to finish"
	cursor=${result##*cursor=}
	result=$(zpool relocate -b 2M -c "$cursor" "$TESTPOOL" \
	    "$work/special") || log_fail "Relocation failed: $result"
done
log_must zpool export "$TESTPOOL"
log_must eval "zdb -e -p '$work' -TTTT '$TESTPOOL' > '$work/brt'"
awk '
    FNR == NR {
	if ($2 == "->" && $NF == "copies=2") {
	    split($1, key, "@")
	    mapped[key[1]] = 1
	}
	next
    }
    !($1 in mapped) { print "Missing two-copy relocation for", $1; bad = 1 }
    END { exit bad }
    ' "$work/brt" "$work/data-addrs" ||
    log_fail "Encrypted file blocks lost relocation copies"
grep -F "$root" "$work/brt" >/dev/null ||
    log_fail "Encrypted root block $root was not relocated"
while read address; do
	grep -F "$address" "$work/brt" >/dev/null ||
	    log_fail "Encrypted indirect block $address was not relocated"
done < "$work/indirect"
while read address; do
	grep -F "$address" "$work/brt" >/dev/null ||
	    log_fail "Encrypted dnode block $address was not relocated"
done < "$work/dnode-addrs"
log_must zpool import -d "$work" "$TESTPOOL"
log_must eval "zfs send -w '$ds@hold' > '$work/after.stream'"
log_must cmp "$work/before.stream" "$work/after.stream"
log_must eval "zfs receive -u -o mountpoint='$work/received' \
    '$TESTPOOL/received' < '$work/after.stream'"
log_must zfs load-key -L "file://$work/key" "$TESTPOOL/received"
log_must zfs mount "$TESTPOOL/received"
log_must cmp "$work/expected" "$work/received/source"
log_must cmp "$work/expected" "$work/received/clone"
log_must zfs destroy -r "$TESTPOOL/received"
log_must zfs load-key "$ds"
log_must zfs mount "$ds"
log_must cmp "$work/expected" "$mnt/source"
log_must cmp "$work/expected" "$mnt/clone"
log_must cmp "$work/expected" "$mnt/.zfs/snapshot/hold/source"
log_must zpool scrub -w "$TESTPOOL"
log_must check_pool_status "$TESTPOOL" "errors" "No known data errors"
log_must zfs destroy "$ds@hold"
log_must rm "$mnt/source" "$mnt/clone"
sync_pool "$TESTPOOL"
log_must zpool wait -t free "$TESTPOOL"
sync_pool "$TESTPOOL"
log_must zpool export "$TESTPOOL"
log_must zdb -e -p "$work" -bcc "$TESTPOOL"

log_pass "Encrypted blocks survived relocation and their final frees"
