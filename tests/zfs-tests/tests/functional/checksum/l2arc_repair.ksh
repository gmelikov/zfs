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

typeset pool=$TESTPOOL2
typeset vdev="$TESTDIR/l2arc_repair.vdev"
typeset cache="$TESTDIR/l2arc_repair.cache"
typeset original="$TESTDIR/l2arc_repair.original"
typeset key="$TESTDIR/l2arc_repair.key"
typeset file="$TESTDIR/l2arc_repair/data"
typeset legacy=$(get_tunable SCAN_LEGACY)
typeset carc=$(get_tunable COMPRESSED_ARC_ENABLED)
typeset rebuild=$(get_tunable L2ARC_REBUILD_ENABLED)
typeset minsize=$(get_tunable L2ARC_REBUILD_BLOCKS_MIN_L2SIZE)
typeset mfuonly=$(get_tunable L2ARC_MFUONLY)
typeset dbuf_shift=$(get_tunable DBUF_CACHE_SHIFT)
typeset injection

function cleanup
{
	[[ -n $injection ]] && log_must zinject -c $injection
	destroy_pool $pool
	log_must set_tunable32 SCAN_LEGACY $legacy
	log_must set_tunable64 COMPRESSED_ARC_ENABLED $carc
	log_must set_tunable32 L2ARC_REBUILD_ENABLED $rebuild
	log_must set_tunable64 L2ARC_REBUILD_BLOCKS_MIN_L2SIZE $minsize
	log_must set_tunable32 L2ARC_MFUONLY $mfuonly
	log_must set_tunable32 DBUF_CACHE_SHIFT $dbuf_shift
	rm -f "$vdev" "$cache" "$original" "$key"
}
log_onexit cleanup

function wait_for_l2
{
	typeset before=$1
	typeset previous=-1
	typeset stable=0
	typeset current
	typeset i

	for ((i = 0; i < 60; i++)); do
		sleep 1
		current=$(kstat arcstats.l2_write_bytes)
		if ((current > before && current == previous)); then
			((stable += 1))
			((stable >= 3)) && return
		else
			stable=0
		fi
		previous=$current
	done
	zpool iostat -v $pool
	log_fail "L2ARC did not finish caching the test data"
}

function drop_l1
{
	log_must zpool export $pool
	typeset before=$(kstat arcstats.l2_rebuild_success)
	typeset i
	log_must zpool import -d "$vdev" -d "$cache" $pool
	for ((i = 0; i < 60; i++)); do
		(( $(kstat arcstats.l2_rebuild_success) > before )) && return
		sleep 1
	done
	zpool status $pool
	log_fail "L2ARC did not rebuild after import"
}

function prepare # compression encryption
{
	typeset -a crypt_opts
	crypt_opts=()
	if [[ $2 == on ]]; then
		crypt_opts=(-o encryption=on -o keyformat=passphrase
		    -o keylocation="file://$key")
	fi
	log_must truncate -s $MINVDEVSIZE "$vdev" "$cache"
	log_must zpool create -f -o ashift=12 -O mountpoint=none \
	    $pool "$vdev" cache "$cache"
	typeset before=$(kstat arcstats.l2_write_bytes)
	log_must zfs create "${crypt_opts[@]}" -o copies=2 \
	    -o mountpoint="$TESTDIR/l2arc_repair" -o compression=$1 \
	    -o checksum=fletcher4 $pool/fs
	log_must cp "$original" "$file"
	log_must sync_pool $pool
	log_must cat "$file" > /dev/null
	wait_for_l2 $before
	# Commit the log block containing the target before export.
	before=$(kstat arcstats.l2_write_bytes)
	log_must dd if=/dev/urandom of="${file%/*}/filler" bs=128k count=32
	log_must sync_pool $pool
	log_must cat "${file%/*}/filler" > /dev/null
	wait_for_l2 $before
	typeset blocks=$(zdb -l "$cache" | awk '/log_blk_count/ {print $2; exit}')
	log_must test "$blocks" -gt 0
	log_must zfs set primarycache=metadata $pool/fs
}

function check_repaired
{
	log_must check_pool_status $pool scan "with 0 errors" true
	log_must check_pool_status $pool errors "No known data errors" true
}

log_assert "Scrub repairs corruption from verified L2ARC data without L1"
log_must set_tunable32 L2ARC_REBUILD_ENABLED 1
log_must set_tunable64 L2ARC_REBUILD_BLOCKS_MIN_L2SIZE 0
log_must set_tunable32 L2ARC_MFUONLY 0
# Release dbuf references so the feed thread can see the ARC data headers.
log_must set_tunable32 DBUF_CACHE_SHIFT 32
log_must dd if=/dev/urandom of="$original" bs=64k count=1
log_must dd if=/dev/zero of="$original" bs=64k count=1 seek=1 conv=notrunc
print "L2ARC test passphrase" > "$key"

for compressed_arc in 0 1; do
	log_must set_tunable64 COMPRESSED_ARC_ENABLED $compressed_arc
	for scan_mode in 0 1; do
		log_must set_tunable32 SCAN_LEGACY $scan_mode
		for compression in off lz4; do
			for encryption in off on; do
				log_note "carc=$compressed_arc legacy=$scan_mode compression=$compression encryption=$encryption"
				prepare $compression $encryption
				log_must corrupt_blocks_at_level "$file"
				drop_l1
				typeset reads=$(kstat arcstats.l2_read_bytes)
				log_must zpool scrub -w $pool
				check_repaired
				log_mustnot check_pool_status $pool scan \
				    "repaired 0B" true
				log_must test $(kstat arcstats.l2_read_bytes) -gt $reads

				# Remove both caches before verifying the disk repair.
				log_must zpool remove $pool "$cache"
				log_must zpool export $pool
				log_must zpool import -d "$vdev" $pool
				if [[ $encryption == on ]]; then
					log_must zfs load-key $pool/fs
					log_must zfs mount $pool/fs
				fi
				log_must cmp "$original" "$file"
				log_must zpool scrub -w $pool
				check_repaired
				log_must check_pool_status $pool scan "repaired 0B" true
				log_must zpool destroy $pool
			done
		done
	done
done

for fault in io corrupt offline; do
	typeset stat=l2_io_error
	[[ $fault == corrupt ]] && stat=l2_cksum_bad
	prepare lz4 off
	drop_l1
	if [[ $fault != offline ]]; then
		injection=$(zinject -q -d "$cache" -e $fault -T read -f 100 $pool) ||
		    log_fail "Could not inject a cache $fault error"
		# Ordinary reads must still fall back to the healthy pool.
		log_must cmp "$original" "$file"
		log_must zinject -c $injection
		injection=
	fi
	log_must corrupt_blocks_at_level "$file"
	drop_l1
	if [[ $fault == offline ]]; then
		log_must zpool offline $pool "$cache"
	else
		injection=$(zinject -q -d "$cache" -e $fault -T read -f 100 $pool) ||
		    log_fail "Could not inject a cache $fault error"
	fi
	typeset errors=$(kstat arcstats.$stat)
	log_must zpool scrub -w $pool
	if [[ $fault != offline ]]; then
		log_must test $(kstat arcstats.$stat) -gt $errors
	fi
	log_must check_pool_status $pool scan "with [1-9][0-9]* errors" true
	log_must check_pool_status $pool scan "repaired 0B" true
	log_mustnot check_pool_status $pool errors "No known data errors" true
	if [[ -n $injection ]]; then
		log_must zinject -c $injection
		injection=
	fi
	log_must zpool destroy $pool
done

log_pass "L2ARC repaired disk corruption only when its raw data was valid"
