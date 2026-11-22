// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2020, 2021, 2022 by Pawel Jakub Dawidek
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/brt.h>
#include <sys/brt_impl.h>
#include <sys/ddt.h>
#include <sys/bitmap.h>
#include <sys/zap.h>
#include <sys/dmu_tx.h>
#include <sys/arc.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/vdev_impl.h>
#include <sys/kstat.h>
#include <sys/metaslab_impl.h>
#include <cityhash.h>
#include <sys/wmsum.h>

/*
 * Block Cloning design.
 *
 * Block Cloning allows to manually clone a file (or a subset of its blocks)
 * into another (or the same) file by just creating additional references to
 * the data blocks without copying the data itself. Those references are kept
 * in the Block Reference Tables (BRTs).
 *
 * In many ways this is similar to the existing deduplication, but there are
 * some important differences:
 *
 * - Deduplication is automatic and Block Cloning is not - one has to use a
 *   dedicated system call(s) to clone the given file/blocks.
 * - Deduplication keeps all data blocks in its table, even those referenced
 *   just once. Block Cloning creates an entry in its tables only when there
 *   are at least two references to the given data block. If the block was
 *   never explicitly cloned or the second to last reference was dropped,
 *   there will be neither space nor performance overhead.
 * - Deduplication needs data to work - one needs to pass real data to the
 *   write(2) syscall, so hash can be calculated. Block Cloning doesn't require
 *   data, just block pointers to the data, so it is extremely fast, as we pay
 *   neither the cost of reading the data, nor the cost of writing the data -
 *   we operate exclusively on metadata.
 * - If the D (dedup) bit is not set in the block pointer, it means that
 *   the block is not in the dedup table (DDT) and we won't consult the DDT
 *   when we need to free the block. Block Cloning must be consulted on every
 *   free, because we cannot modify the source BP (eg. by setting something
 *   similar to the D bit), thus we have no hint if the block is in the
 *   Block Reference Table (BRT), so we need to look into the BRT. There is
 *   an optimization in place that allows us to eliminate the majority of BRT
 *   lookups which is described below in the "Minimizing free penalty" section.
 * - The BRT entry is much smaller than the DDT entry - for BRT we only store
 *   64bit offset and 64bit reference counter.
 * - Dedup keys are cryptographic hashes, so two blocks that are close to each
 *   other on disk are most likely in totally different parts of the DDT.
 *   The BRT entry keys are offsets into a single top-level VDEV, so data blocks
 *   from one file should have BRT entries close to each other.
 * - Scrub will only do a single pass over a block that is referenced multiple
 *   times in the DDT. Unfortunately it is not currently (if at all) possible
 *   with Block Cloning and block referenced multiple times will be scrubbed
 *   multiple times. The new, sorted scrub should be able to eliminate
 *   duplicated reads given enough memory.
 * - Deduplication requires cryptographically strong hash as a checksum or
 *   additional data verification. Block Cloning works with any checksum
 *   algorithm or even with checksumming disabled.
 *
 * As mentioned above, the BRT entries are much smaller than the DDT entries.
 * To uniquely identify a block we just need its vdev id and offset. We also
 * need to maintain a reference counter. The vdev id will often repeat, as there
 * is a small number of top-level VDEVs and a large number of blocks stored in
 * each VDEV. We take advantage of that to reduce the BRT entry size further by
 * maintaining one BRT for each top-level VDEV, so we can then have only offset
 * and counter as the BRT entry.
 *
 * Minimizing free penalty.
 *
 * Block Cloning allows creating additional references to any existing block.
 * When we free a block there is no hint in the block pointer whether the block
 * was cloned or not, so on each free we have to check if there is a
 * corresponding entry in the BRT or not. If there is, we need to decrease
 * the reference counter. Doing BRT lookup on every free can potentially be
 * expensive by requiring additional I/Os if the BRT doesn't fit into memory.
 * This is the main problem with deduplication, so we've learned our lesson and
 * try not to repeat the same mistake here. How do we do that? We divide each
 * top-level VDEV into 16MB regions. For each region we maintain a count of
 * Ordinary BRT entries that have offsets within the region.
 * This creates the entries count array of 16bit numbers for each top-level
 * VDEV.
 * The entries count array is always kept in memory and updated on disk in the
 * same transaction group as the BRT updates to keep everything in-sync. We can
 * keep the array in memory, because it is very small. With 16MB regions and
 * 1TB VDEV the array requires only 128kB of memory (we may decide to decrease
 * the region size even further in the future). Now, when we want to free
 * a block, we first consult the array. If the counter for the whole region is
 * zero, there is no need to look for the BRT entry, as there isn't one for
 * sure. Relocated entries are looked up separately in the relocation map.
 * If an ordinary entry is found, we decrease its reference counter and the
 * region count.
 *
 * The entry counters array is small, but can potentially be larger for very
 * large VDEVs or smaller regions. In this case we don't want to rewrite entire
 * array on every change. We then divide the array into 32kB block and keep
 * a bitmap of dirty blocks within a transaction group. When we sync the
 * transaction group we can only update the parts of the entry counters array
 * that were modified. Note: Keeping track of the dirty parts of the entry
 * counters array is implemented, but updating only parts of the array on disk
 * is not yet implemented - for now we will update entire array if there was
 * any change.
 *
 * The implementation tries to be economic: if BRT is not used, or no longer
 * used, there will be no entries in the MOS and no additional memory used (eg.
 * the entry counters array is only allocated if needed).
 *
 * Interaction between Deduplication and Block Cloning.
 *
 * If both functionalities are in use, we could end up with a block that is
 * referenced multiple times in both DDT and BRT. When we free one of the
 * references we couldn't tell where it belongs, so we would have to decide
 * what table takes the precedence: do we first clear DDT references or BRT
 * references? To avoid this dilemma BRT cooperates with DDT - if a given block
 * is being cloned using BRT and the BP has the D (dedup) bit set, BRT will
 * lookup DDT entry instead and increase the counter there. No BRT entry
 * will be created for a block which has the D (dedup) bit set.
 * BRT may be more efficient for manual deduplication, but if the block is
 * already in the DDT, then creating additional BRT entry would be less
 * efficient. This clever idea was proposed by Allan Jude.
 *
 * Block Cloning across datasets.
 *
 * Block Cloning is not limited to cloning blocks within the same dataset.
 * It is possible (and very useful) to clone blocks between different datasets.
 * One use case is recovering files from snapshots. By cloning the files into
 * dataset we need no additional storage. Without Block Cloning we would need
 * additional space for those files.
 * Another interesting use case is moving the files between datasets
 * (copying the file content to the new dataset and removing the source file).
 * In that case Block Cloning will only be used briefly, because the BRT entries
 * will be removed when the source is removed.
 * Block Cloning across encrypted datasets is supported as long as both
 * datasets share the same master key (e.g. snapshots and clones)
 *
 * Block Cloning flow through ZFS layers.
 *
 * Note: Block Cloning can be used both for cloning file system blocks and ZVOL
 * blocks. As of this writing no interface is implemented that allows for block
 * cloning within a ZVOL.
 * FreeBSD and Linux provides copy_file_range(2) system call and we will use it
 * for blocking cloning.
 *
 *	ssize_t
 *	copy_file_range(int infd, off_t *inoffp, int outfd, off_t *outoffp,
 *	                size_t len, unsigned int flags);
 *
 * Even though offsets and length represent bytes, they have to be
 * block-aligned or we will return an error so the upper layer can
 * fallback to the generic mechanism that will just copy the data.
 * Using copy_file_range(2) will call OS-independent zfs_clone_range() function.
 * This function was implemented based on zfs_write(), but instead of writing
 * the given data we first read block pointers using the new dmu_read_l0_bps()
 * function from the source file. Once we have BPs from the source file we call
 * the dmu_brt_clone() function on the destination file. This function
 * allocates BPs for us. We iterate over all source BPs. If the given BP is
 * a hole or an embedded block, we just copy BP as-is. If it points to a real
 * data we place this BP on a BRT pending list using the brt_pending_add()
 * function.
 *
 * We use this pending list to keep track of all BPs that got new references
 * within this transaction group.
 *
 * Some special cases to consider and how we address them:
 * - The block we want to clone may have been created within the same
 *   transaction group that we are trying to clone. Such block has no BP
 *   allocated yet, so cannot be immediately cloned. We return EAGAIN.
 * - The block we want to clone may have been modified within the same
 *   transaction group. We return EAGAIN.
 * - A block may be cloned multiple times during one transaction group (that's
 *   why pending list is actually a tree and not an append-only list - this
 *   way we can figure out faster if this block is cloned for the first time
 *   in this txg or consecutive time).
 * - A block may be cloned and freed within the same transaction group
 *   (see dbuf_undirty()).
 * - A block may be cloned and within the same transaction group the clone
 *   can be cloned again (see dmu_read_l0_bps()).
 * - A file might have been deleted, but the caller still has a file descriptor
 *   open to this file and clones it.
 *
 * When we free a block we have an additional step in the ZIO pipeline where we
 * call the zio_brt_free() function. We then call the brt_entry_decref()
 * that loads the corresponding BRT entry (if one exists) and decreases
 * reference counter. If this is not the last reference we will stop ZIO
 * pipeline here. If this is the last reference or the block is not in the
 * BRT, we continue the pipeline and free the block as usual.
 *
 * At the beginning of spa_sync() where there can be no more block cloning,
 * but before issuing frees we call brt_pending_apply(). This function applies
 * all the new clones to the BRT table - we load BRT entries and update
 * reference counters. Blocks with the DEDUP bit set are referenced in the
 * DDT instead and are kept on separate pending trees, sorted and sharded by
 * the DDT ZAP hash, so that syncing context can process them in parallel
 * with sequential access to the DDT ZAP leaves. To sync new BRT entries to
 * disk, we use brt_sync() function. This function will sync all dirty
 * per-top-level-vdev BRTs, the entry counters arrays, etc.
 *
 * Block Cloning and ZIL.
 *
 * Every clone operation is divided into chunks (similar to write) and each
 * chunk is cloned in a separate transaction. The chunk size is determined by
 * how many BPs we can fit into a single ZIL entry.
 * Replaying clone operation is different from the regular clone operation,
 * as when we log clone operations we cannot use the source object - it may
 * reside on a different dataset, so we log BPs we want to clone.
 * The ZIL is replayed when we mount the given dataset, not when the pool is
 * imported. Taking this into account it is possible that the pool is imported
 * without mounting datasets and the source dataset is destroyed before the
 * destination dataset is mounted and its ZIL replayed.
 * To address this situation we leverage zil_claim() mechanism where ZFS will
 * parse all the ZILs on pool import. When we come across TX_CLONE_RANGE
 * entries, we will bump reference counters for their BPs in the BRT.  Then
 * on mount and ZIL replay we bump the reference counters once more, while the
 * first references are dropped during ZIL destroy by zil_free_clone_range().
 * It is possible that after zil_claim() we never mount the destination, so
 * we never replay its ZIL and just destroy it.  In this case the only taken
 * references will be dropped by zil_free_clone_range(), since the cloning is
 * not going to ever take place.
 */

static kmem_cache_t *brt_entry_cache;

/*
 * Enable/disable prefetching of BRT entries that we are going to modify.
 */
static int brt_zap_prefetch = 1;

#ifdef ZFS_DEBUG
#define	BRT_DEBUG(...)	do {						\
	if ((zfs_flags & ZFS_DEBUG_BRT) != 0) {				\
		__dprintf(B_TRUE, __FILE__, __func__, __LINE__, __VA_ARGS__); \
	}								\
} while (0)
#else
#define	BRT_DEBUG(...)	do { } while (0)
#endif

static int brt_zap_default_bs = 13;
static int brt_zap_default_ibs = 13;

static kstat_t	*brt_ksp;

typedef struct brt_stats {
	kstat_named_t brt_addref_entry_not_on_disk;
	kstat_named_t brt_addref_entry_on_disk;
	kstat_named_t brt_decref_entry_in_memory;
	kstat_named_t brt_decref_entry_loaded_from_disk;
	kstat_named_t brt_decref_entry_not_in_memory;
	kstat_named_t brt_decref_entry_read_lost_race;
	kstat_named_t brt_decref_entry_still_referenced;
	kstat_named_t brt_decref_free_data_later;
	kstat_named_t brt_decref_free_data_now;
	kstat_named_t brt_decref_no_entry;
} brt_stats_t;

static brt_stats_t brt_stats = {
	{ "addref_entry_not_on_disk",		KSTAT_DATA_UINT64 },
	{ "addref_entry_on_disk",		KSTAT_DATA_UINT64 },
	{ "decref_entry_in_memory",		KSTAT_DATA_UINT64 },
	{ "decref_entry_loaded_from_disk",	KSTAT_DATA_UINT64 },
	{ "decref_entry_not_in_memory",		KSTAT_DATA_UINT64 },
	{ "decref_entry_read_lost_race",	KSTAT_DATA_UINT64 },
	{ "decref_entry_still_referenced",	KSTAT_DATA_UINT64 },
	{ "decref_free_data_later",		KSTAT_DATA_UINT64 },
	{ "decref_free_data_now",		KSTAT_DATA_UINT64 },
	{ "decref_no_entry",			KSTAT_DATA_UINT64 }
};

struct {
	wmsum_t brt_addref_entry_not_on_disk;
	wmsum_t brt_addref_entry_on_disk;
	wmsum_t brt_decref_entry_in_memory;
	wmsum_t brt_decref_entry_loaded_from_disk;
	wmsum_t brt_decref_entry_not_in_memory;
	wmsum_t brt_decref_entry_read_lost_race;
	wmsum_t brt_decref_entry_still_referenced;
	wmsum_t brt_decref_free_data_later;
	wmsum_t brt_decref_free_data_now;
	wmsum_t brt_decref_no_entry;
} brt_sums;

#define	BRTSTAT_BUMP(stat)	wmsum_add(&brt_sums.stat, 1)

static int brt_entry_compare(const void *x1, const void *x2);
static void brt_vdevs_expand(spa_t *spa, uint64_t nvdevs);

static void
brt_rlock(spa_t *spa)
{
	rw_enter(&spa->spa_brt_lock, RW_READER);
}

static void
brt_wlock(spa_t *spa)
{
	rw_enter(&spa->spa_brt_lock, RW_WRITER);
}

static void
brt_unlock(spa_t *spa)
{
	rw_exit(&spa->spa_brt_lock);
}

static uint16_t
brt_vdev_entcount_get(const brt_vdev_t *brtvd, uint64_t idx)
{

	ASSERT3U(idx, <, brtvd->bv_size);

	if (unlikely(brtvd->bv_need_byteswap)) {
		return (BSWAP_16(brtvd->bv_entcount[idx]));
	} else {
		return (brtvd->bv_entcount[idx]);
	}
}

static void
brt_vdev_entcount_set(brt_vdev_t *brtvd, uint64_t idx, uint16_t entcnt)
{

	ASSERT3U(idx, <, brtvd->bv_size);

	if (unlikely(brtvd->bv_need_byteswap)) {
		brtvd->bv_entcount[idx] = BSWAP_16(entcnt);
	} else {
		brtvd->bv_entcount[idx] = entcnt;
	}
}

static void
brt_vdev_entcount_inc(brt_vdev_t *brtvd, uint64_t idx)
{
	uint16_t entcnt;

	ASSERT3U(idx, <, brtvd->bv_size);

	entcnt = brt_vdev_entcount_get(brtvd, idx);
	ASSERT(entcnt < UINT16_MAX);
	brt_vdev_entcount_set(brtvd, idx, entcnt + 1);
}

static void
brt_vdev_entcount_dec(brt_vdev_t *brtvd, uint64_t idx)
{
	uint16_t entcnt;

	ASSERT3U(idx, <, brtvd->bv_size);

	entcnt = brt_vdev_entcount_get(brtvd, idx);
	ASSERT(entcnt > 0);
	brt_vdev_entcount_set(brtvd, idx, entcnt - 1);
}

#ifdef ZFS_DEBUG
static void
brt_vdev_dump(brt_vdev_t *brtvd)
{
	uint64_t idx;

	uint64_t nblocks = BRT_RANGESIZE_TO_NBLOCKS(brtvd->bv_size);
	zfs_dbgmsg("  BRT vdevid=%llu meta_dirty=%d entcount_dirty=%d "
	    "size=%llu totalcount=%llu nblocks=%llu bitmapsize=%zu",
	    (u_longlong_t)brtvd->bv_vdevid,
	    brtvd->bv_meta_dirty, brtvd->bv_entcount_dirty,
	    (u_longlong_t)brtvd->bv_size,
	    (u_longlong_t)brtvd->bv_totalcount,
	    (u_longlong_t)nblocks,
	    (size_t)BT_SIZEOFMAP(nblocks));
	if (brtvd->bv_totalcount > 0) {
		zfs_dbgmsg("    entcounts:");
		for (idx = 0; idx < brtvd->bv_size; idx++) {
			uint16_t entcnt = brt_vdev_entcount_get(brtvd, idx);
			if (entcnt > 0) {
				zfs_dbgmsg("      [%04llu] %hu",
				    (u_longlong_t)idx, entcnt);
			}
		}
	}
	if (brtvd->bv_entcount_dirty) {
		char *bitmap;

		bitmap = kmem_alloc(nblocks + 1, KM_SLEEP);
		for (idx = 0; idx < nblocks; idx++) {
			bitmap[idx] =
			    BT_TEST(brtvd->bv_bitmap, idx) ? 'x' : '.';
		}
		bitmap[idx] = '\0';
		zfs_dbgmsg("    dirty: %s", bitmap);
		kmem_free(bitmap, nblocks + 1);
	}
}
#endif

static brt_vdev_t *
brt_vdev(spa_t *spa, uint64_t vdevid, boolean_t alloc)
{
	brt_vdev_t *brtvd = NULL;

	brt_rlock(spa);
	if (vdevid < spa->spa_brt_nvdevs) {
		brtvd = spa->spa_brt_vdevs[vdevid];
	} else if (alloc) {
		/* New VDEV was added. */
		brt_unlock(spa);
		brt_wlock(spa);
		if (vdevid >= spa->spa_brt_nvdevs)
			brt_vdevs_expand(spa, vdevid + 1);
		brtvd = spa->spa_brt_vdevs[vdevid];
	}
	brt_unlock(spa);
	return (brtvd);
}

static void
brt_vdev_create(spa_t *spa, brt_vdev_t *brtvd, dmu_tx_t *tx)
{
	char name[64];

	ASSERT(brtvd->bv_initiated);
	ASSERT0(brtvd->bv_mos_brtvdev);
	ASSERT0(brtvd->bv_mos_entries);

	uint64_t mos_entries = zap_create_flags(spa->spa_meta_objset, 0,
	    ZAP_FLAG_HASH64 | ZAP_FLAG_UINT64_KEY, DMU_OTN_ZAP_METADATA,
	    brt_zap_default_bs, brt_zap_default_ibs, DMU_OT_NONE, 0, tx);
	VERIFY(mos_entries != 0);
	VERIFY0(dnode_hold(spa->spa_meta_objset, mos_entries, brtvd,
	    &brtvd->bv_mos_entries_dnode));
	dnode_set_storage_type(brtvd->bv_mos_entries_dnode, DMU_OT_DDT_ZAP);
	rw_enter(&brtvd->bv_mos_entries_lock, RW_WRITER);
	brtvd->bv_mos_entries = mos_entries;
	rw_exit(&brtvd->bv_mos_entries_lock);
	BRT_DEBUG("MOS entries created, object=%llu",
	    (u_longlong_t)brtvd->bv_mos_entries);

	/*
	 * We allocate DMU buffer to store the bv_entcount[] array.
	 * We will keep array size (bv_size) and cummulative count for all
	 * bv_entcount[]s (bv_totalcount) in the bonus buffer.
	 */
	brtvd->bv_mos_brtvdev = dmu_object_alloc(spa->spa_meta_objset,
	    DMU_OTN_UINT64_METADATA, BRT_BLOCKSIZE,
	    DMU_OTN_UINT64_METADATA, sizeof (brt_vdev_phys_t), tx);
	VERIFY(brtvd->bv_mos_brtvdev != 0);
	BRT_DEBUG("MOS BRT VDEV created, object=%llu",
	    (u_longlong_t)brtvd->bv_mos_brtvdev);

	snprintf(name, sizeof (name), "%s%llu", BRT_OBJECT_VDEV_PREFIX,
	    (u_longlong_t)brtvd->bv_vdevid);
	VERIFY0(zap_add(spa->spa_meta_objset, DMU_POOL_DIRECTORY_OBJECT, name,
	    sizeof (uint64_t), 1, &brtvd->bv_mos_brtvdev, tx));
	BRT_DEBUG("Pool directory object created, object=%s", name);

	/*
	 * Activate the endian-fixed feature if this is the first BRT ZAP
	 * (i.e., BLOCK_CLONING is not yet active) and the feature is enabled.
	 */
	if (spa_feature_is_enabled(spa, SPA_FEATURE_BLOCK_CLONING_ENDIAN) &&
	    !spa_feature_is_active(spa, SPA_FEATURE_BLOCK_CLONING)) {
		spa_feature_incr(spa, SPA_FEATURE_BLOCK_CLONING_ENDIAN, tx);
	} else if (spa_feature_is_active(spa,
	    SPA_FEATURE_BLOCK_CLONING_ENDIAN)) {
		spa_feature_incr(spa, SPA_FEATURE_BLOCK_CLONING_ENDIAN, tx);
	}

	spa_feature_incr(spa, SPA_FEATURE_BLOCK_CLONING, tx);
}

static void
brt_vdev_realloc(spa_t *spa, brt_vdev_t *brtvd)
{
	vdev_t *vd;
	uint16_t *entcount;
	ulong_t *bitmap;
	uint64_t nblocks, onblocks, size;

	ASSERT(RW_WRITE_HELD(&brtvd->bv_lock));

	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vd = vdev_lookup_top(spa, brtvd->bv_vdevid);
	size = (vdev_get_min_asize(vd) - 1) / spa->spa_brt_rangesize + 1;
	spa_config_exit(spa, SCL_VDEV, FTAG);

	nblocks = BRT_RANGESIZE_TO_NBLOCKS(size);
	entcount = vmem_zalloc(nblocks * BRT_BLOCKSIZE, KM_SLEEP);
	bitmap = kmem_zalloc(BT_SIZEOFMAP(nblocks), KM_SLEEP);

	if (!brtvd->bv_initiated) {
		ASSERT0(brtvd->bv_size);
		ASSERT0P(brtvd->bv_entcount);
		ASSERT0P(brtvd->bv_bitmap);
	} else {
		ASSERT(brtvd->bv_size > 0);
		ASSERT(brtvd->bv_entcount != NULL);
		ASSERT(brtvd->bv_bitmap != NULL);
		/*
		 * TODO: Allow vdev shrinking. We only need to implement
		 * shrinking the on-disk BRT VDEV object.
		 * dmu_free_range(spa->spa_meta_objset, brtvd->bv_mos_brtvdev,
		 *     offset, size, tx);
		 */
		ASSERT3U(brtvd->bv_size, <=, size);

		memcpy(entcount, brtvd->bv_entcount,
		    sizeof (entcount[0]) * MIN(size, brtvd->bv_size));
		onblocks = BRT_RANGESIZE_TO_NBLOCKS(brtvd->bv_size);
		vmem_free(brtvd->bv_entcount, onblocks * BRT_BLOCKSIZE);
		memcpy(bitmap, brtvd->bv_bitmap, MIN(BT_SIZEOFMAP(nblocks),
		    BT_SIZEOFMAP(onblocks)));
		kmem_free(brtvd->bv_bitmap, BT_SIZEOFMAP(onblocks));
	}

	brtvd->bv_size = size;
	brtvd->bv_entcount = entcount;
	brtvd->bv_bitmap = bitmap;
	if (!brtvd->bv_initiated) {
		brtvd->bv_need_byteswap = FALSE;
		brtvd->bv_initiated = TRUE;
		BRT_DEBUG("BRT VDEV %llu initiated.",
		    (u_longlong_t)brtvd->bv_vdevid);
	}
}

static int
brt_vdev_load(spa_t *spa, brt_vdev_t *brtvd)
{
	dmu_buf_t *db;
	brt_vdev_phys_t *bvphys;
	int error;

	ASSERT(!brtvd->bv_initiated);
	ASSERT(brtvd->bv_mos_brtvdev != 0);

	error = dmu_bonus_hold(spa->spa_meta_objset, brtvd->bv_mos_brtvdev,
	    FTAG, &db);
	if (error != 0)
		return (error);

	bvphys = db->db_data;
	if (spa->spa_brt_rangesize == 0) {
		spa->spa_brt_rangesize = bvphys->bvp_rangesize;
	} else {
		ASSERT3U(spa->spa_brt_rangesize, ==, bvphys->bvp_rangesize);
	}

	brt_vdev_realloc(spa, brtvd);

	/* TODO: We don't support VDEV shrinking. */
	ASSERT3U(bvphys->bvp_size, <=, brtvd->bv_size);

	/*
	 * If VDEV grew, we will leave new bv_entcount[] entries zeroed out.
	 */
	error = dmu_read(spa->spa_meta_objset, brtvd->bv_mos_brtvdev, 0,
	    MIN(brtvd->bv_size, bvphys->bvp_size) * sizeof (uint16_t),
	    brtvd->bv_entcount, DMU_READ_NO_PREFETCH | DMU_UNCACHEDIO);
	if (error != 0) {
		dmu_buf_rele(db, FTAG);
		return (error);
	}

	ASSERT(bvphys->bvp_mos_entries != 0);
	VERIFY0(dnode_hold(spa->spa_meta_objset, bvphys->bvp_mos_entries, brtvd,
	    &brtvd->bv_mos_entries_dnode));
	dnode_set_storage_type(brtvd->bv_mos_entries_dnode, DMU_OT_DDT_ZAP);
	rw_enter(&brtvd->bv_mos_entries_lock, RW_WRITER);
	brtvd->bv_mos_entries = bvphys->bvp_mos_entries;
	rw_exit(&brtvd->bv_mos_entries_lock);
	brtvd->bv_need_byteswap =
	    (bvphys->bvp_byteorder != BRT_NATIVE_BYTEORDER);
	brtvd->bv_totalcount = bvphys->bvp_totalcount;
	brtvd->bv_usedspace = bvphys->bvp_usedspace;
	brtvd->bv_savedspace = bvphys->bvp_savedspace;

	dmu_buf_rele(db, FTAG);

	BRT_DEBUG("BRT VDEV %llu loaded: mos_brtvdev=%llu, mos_entries=%llu",
	    (u_longlong_t)brtvd->bv_vdevid,
	    (u_longlong_t)brtvd->bv_mos_brtvdev,
	    (u_longlong_t)brtvd->bv_mos_entries);
	return (0);
}

static void
brt_vdev_dealloc(brt_vdev_t *brtvd)
{
	ASSERT(RW_WRITE_HELD(&brtvd->bv_lock));
	ASSERT(brtvd->bv_initiated);
	ASSERT0(avl_numnodes(&brtvd->bv_tree));

	uint64_t nblocks = BRT_RANGESIZE_TO_NBLOCKS(brtvd->bv_size);
	vmem_free(brtvd->bv_entcount, nblocks * BRT_BLOCKSIZE);
	brtvd->bv_entcount = NULL;
	kmem_free(brtvd->bv_bitmap, BT_SIZEOFMAP(nblocks));
	brtvd->bv_bitmap = NULL;

	brtvd->bv_size = 0;

	brtvd->bv_initiated = FALSE;
	BRT_DEBUG("BRT VDEV %llu deallocated.", (u_longlong_t)brtvd->bv_vdevid);
}

static void
brt_vdev_destroy(spa_t *spa, brt_vdev_t *brtvd, dmu_tx_t *tx)
{
	char name[64];
	uint64_t count;

	ASSERT(brtvd->bv_initiated);
	ASSERT(brtvd->bv_mos_brtvdev != 0);
	ASSERT(brtvd->bv_mos_entries != 0);
	ASSERT0(brtvd->bv_totalcount);
	ASSERT0(brtvd->bv_usedspace);
	ASSERT0(brtvd->bv_savedspace);

	uint64_t mos_entries = brtvd->bv_mos_entries;
	rw_enter(&brtvd->bv_mos_entries_lock, RW_WRITER);
	brtvd->bv_mos_entries = 0;
	rw_exit(&brtvd->bv_mos_entries_lock);
	dnode_rele(brtvd->bv_mos_entries_dnode, brtvd);
	brtvd->bv_mos_entries_dnode = NULL;
	ASSERT0(zap_count(spa->spa_meta_objset, mos_entries, &count));
	ASSERT0(count);
	VERIFY0(zap_destroy(spa->spa_meta_objset, mos_entries, tx));
	BRT_DEBUG("MOS entries destroyed, object=%llu",
	    (u_longlong_t)mos_entries);

	VERIFY0(dmu_object_free(spa->spa_meta_objset, brtvd->bv_mos_brtvdev,
	    tx));
	BRT_DEBUG("MOS BRT VDEV destroyed, object=%llu",
	    (u_longlong_t)brtvd->bv_mos_brtvdev);
	brtvd->bv_mos_brtvdev = 0;
	brtvd->bv_entcount_dirty = FALSE;

	snprintf(name, sizeof (name), "%s%llu", BRT_OBJECT_VDEV_PREFIX,
	    (u_longlong_t)brtvd->bv_vdevid);
	VERIFY0(zap_remove(spa->spa_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    name, tx));
	BRT_DEBUG("Pool directory object removed, object=%s", name);

	brtvd->bv_meta_dirty = FALSE;

	rw_enter(&brtvd->bv_lock, RW_WRITER);
	brt_vdev_dealloc(brtvd);
	rw_exit(&brtvd->bv_lock);

	spa_feature_decr(spa, SPA_FEATURE_BLOCK_CLONING, tx);
	if (spa_feature_is_active(spa, SPA_FEATURE_BLOCK_CLONING_ENDIAN))
		spa_feature_decr(spa, SPA_FEATURE_BLOCK_CLONING_ENDIAN, tx);
}

static void
brt_vdevs_expand(spa_t *spa, uint64_t nvdevs)
{
	brt_vdev_t **vdevs;

	ASSERT(RW_WRITE_HELD(&spa->spa_brt_lock));
	ASSERT3U(nvdevs, >=, spa->spa_brt_nvdevs);

	if (nvdevs == spa->spa_brt_nvdevs)
		return;

	vdevs = kmem_zalloc(sizeof (*spa->spa_brt_vdevs) * nvdevs, KM_SLEEP);
	if (spa->spa_brt_nvdevs > 0) {
		ASSERT(spa->spa_brt_vdevs != NULL);

		memcpy(vdevs, spa->spa_brt_vdevs,
		    sizeof (*spa->spa_brt_vdevs) * spa->spa_brt_nvdevs);
		kmem_free(spa->spa_brt_vdevs,
		    sizeof (*spa->spa_brt_vdevs) * spa->spa_brt_nvdevs);
	}
	spa->spa_brt_vdevs = vdevs;

	for (uint64_t vdevid = spa->spa_brt_nvdevs; vdevid < nvdevs; vdevid++) {
		brt_vdev_t *brtvd = kmem_zalloc(sizeof (*brtvd), KM_SLEEP);
		rw_init(&brtvd->bv_lock, NULL, RW_DEFAULT, NULL);
		brtvd->bv_vdevid = vdevid;
		brtvd->bv_initiated = FALSE;
		rw_init(&brtvd->bv_mos_entries_lock, NULL, RW_DEFAULT, NULL);
		avl_create(&brtvd->bv_tree, brt_entry_compare,
		    sizeof (brt_entry_t), offsetof(brt_entry_t, bre_node));
		for (int i = 0; i < TXG_SIZE; i++) {
			avl_create(&brtvd->bv_pending_tree[i],
			    brt_entry_compare, sizeof (brt_entry_t),
			    offsetof(brt_entry_t, bre_node));
		}
		mutex_init(&brtvd->bv_pending_lock, NULL, MUTEX_DEFAULT, NULL);
		spa->spa_brt_vdevs[vdevid] = brtvd;
	}

	BRT_DEBUG("BRT VDEVs expanded from %llu to %llu.",
	    (u_longlong_t)spa->spa_brt_nvdevs, (u_longlong_t)nvdevs);
	spa->spa_brt_nvdevs = nvdevs;
}

static boolean_t
brt_vdev_lookup(spa_t *spa, brt_vdev_t *brtvd, uint64_t offset)
{
	uint64_t idx = offset / spa->spa_brt_rangesize;
	if (idx < brtvd->bv_size) {
		/* VDEV wasn't expanded. */
		return (brt_vdev_entcount_get(brtvd, idx) > 0);
	}
	return (FALSE);
}

static void
brt_vdev_addref(spa_t *spa, brt_vdev_t *brtvd, const brt_entry_t *bre,
    uint64_t dsize, uint64_t count)
{
	uint64_t idx;
	boolean_t relocated = brt_reloc_resolve(spa, &bre->bre_bp, NULL);

	ASSERT(brtvd->bv_initiated);

	brtvd->bv_savedspace += dsize * count;
	brtvd->bv_meta_dirty = TRUE;

	if (bre->bre_count > 0)
		return;

	brtvd->bv_usedspace += dsize;

	idx = BRE_OFFSET(bre) / spa->spa_brt_rangesize;
	if (idx >= brtvd->bv_size) {
		/* VDEV has been expanded. */
		rw_enter(&brtvd->bv_lock, RW_WRITER);
		brt_vdev_realloc(spa, brtvd);
		rw_exit(&brtvd->bv_lock);
	}

	ASSERT3U(idx, <, brtvd->bv_size);

	brtvd->bv_totalcount++;
	if (!relocated) {
		brt_vdev_entcount_inc(brtvd, idx);
		brtvd->bv_entcount_dirty = TRUE;
		BT_SET(brtvd->bv_bitmap,
		    idx / (BRT_BLOCKSIZE / sizeof (uint16_t)));
	}
}

static void
brt_vdev_decref(spa_t *spa, brt_vdev_t *brtvd, const brt_entry_t *bre,
    uint64_t dsize, boolean_t relocated)
{
	uint64_t idx;

	ASSERT(RW_WRITE_HELD(&brtvd->bv_lock));
	ASSERT(brtvd->bv_initiated);

	brtvd->bv_savedspace -= dsize;
	brtvd->bv_meta_dirty = TRUE;

	if (bre->bre_count > 0)
		return;

	brtvd->bv_usedspace -= dsize;

	idx = BRE_OFFSET(bre) / spa->spa_brt_rangesize;
	ASSERT3U(idx, <, brtvd->bv_size);

	ASSERT(brtvd->bv_totalcount > 0);
	brtvd->bv_totalcount--;
	if (!relocated) {
		brt_vdev_entcount_dec(brtvd, idx);
		brtvd->bv_entcount_dirty = TRUE;
		BT_SET(brtvd->bv_bitmap,
		    idx / (BRT_BLOCKSIZE / sizeof (uint16_t)));
	}
}

static void
brt_vdev_sync(spa_t *spa, brt_vdev_t *brtvd, dmu_tx_t *tx)
{
	dmu_buf_t *db;
	brt_vdev_phys_t *bvphys;

	ASSERT(brtvd->bv_meta_dirty);
	ASSERT(brtvd->bv_mos_brtvdev != 0);
	ASSERT(dmu_tx_is_syncing(tx));

	VERIFY0(dmu_bonus_hold(spa->spa_meta_objset, brtvd->bv_mos_brtvdev,
	    FTAG, &db));

	if (brtvd->bv_entcount_dirty) {
		uint64_t nblocks = BRT_RANGESIZE_TO_NBLOCKS(brtvd->bv_size);
		for (uint64_t i = 0; i < nblocks; i++) {
			if (!BT_TEST(brtvd->bv_bitmap, i))
				continue;
			uint64_t end = i + 1;
			uint64_t maxend = MIN(i + DMU_MAX_ACCESS / 2 /
			    BRT_BLOCKSIZE, nblocks);
			while (end < maxend && BT_TEST(brtvd->bv_bitmap, end))
				end++;
			dmu_write(spa->spa_meta_objset, brtvd->bv_mos_brtvdev,
			    i * BRT_BLOCKSIZE, (end - i) * BRT_BLOCKSIZE,
			    (char *)brtvd->bv_entcount + i * BRT_BLOCKSIZE,
			    tx, DMU_READ_NO_PREFETCH | DMU_UNCACHEDIO);
			i = end - 1;
		}
		memset(brtvd->bv_bitmap, 0, BT_SIZEOFMAP(nblocks));
		brtvd->bv_entcount_dirty = FALSE;
	}

	dmu_buf_will_dirty(db, tx);
	bvphys = db->db_data;
	bvphys->bvp_mos_entries = brtvd->bv_mos_entries;
	bvphys->bvp_size = brtvd->bv_size;
	if (brtvd->bv_need_byteswap) {
		bvphys->bvp_byteorder = BRT_NON_NATIVE_BYTEORDER;
	} else {
		bvphys->bvp_byteorder = BRT_NATIVE_BYTEORDER;
	}
	bvphys->bvp_totalcount = brtvd->bv_totalcount;
	bvphys->bvp_rangesize = spa->spa_brt_rangesize;
	bvphys->bvp_usedspace = brtvd->bv_usedspace;
	bvphys->bvp_savedspace = brtvd->bv_savedspace;
	dmu_buf_rele(db, FTAG);

	brtvd->bv_meta_dirty = FALSE;
}

static void
brt_vdevs_free(spa_t *spa)
{
	if (spa->spa_brt_vdevs == 0)
		return;
	for (uint64_t vdevid = 0; vdevid < spa->spa_brt_nvdevs; vdevid++) {
		brt_vdev_t *brtvd = spa->spa_brt_vdevs[vdevid];
		rw_enter(&brtvd->bv_lock, RW_WRITER);
		if (brtvd->bv_initiated)
			brt_vdev_dealloc(brtvd);
		rw_exit(&brtvd->bv_lock);
		rw_destroy(&brtvd->bv_lock);
		if (brtvd->bv_mos_entries != 0)
			dnode_rele(brtvd->bv_mos_entries_dnode, brtvd);
		rw_destroy(&brtvd->bv_mos_entries_lock);
		avl_destroy(&brtvd->bv_tree);
		for (int i = 0; i < TXG_SIZE; i++)
			avl_destroy(&brtvd->bv_pending_tree[i]);
		mutex_destroy(&brtvd->bv_pending_lock);
		kmem_free(brtvd, sizeof (*brtvd));
	}
	kmem_free(spa->spa_brt_vdevs, sizeof (*spa->spa_brt_vdevs) *
	    spa->spa_brt_nvdevs);
}

static void
brt_entry_fill(const blkptr_t *bp, brt_entry_t *bre, uint64_t *vdevidp)
{

	bre->bre_bp = *bp;
	bre->bre_count = 0;
	bre->bre_pcount = 0;

	*vdevidp = DVA_GET_VDEV(&bp->blk_dva[0]);
}

static boolean_t
brt_has_endian_fixed(spa_t *spa)
{
	return (spa_feature_is_active(spa, SPA_FEATURE_BLOCK_CLONING_ENDIAN));
}

/* DVA offsets are aligned to SPA_MINBLOCKSIZE. */
#define	BRT_RELOC_EXTRA		(1ULL << 0)
#define	BRT_RELOC_QUEUED	(1ULL << 1)
#define	BRT_RELOC_RETIRING	(1ULL << 2)
#define	BRT_RELOC_FLAGS		(BRT_RELOC_EXTRA | BRT_RELOC_QUEUED | \
	BRT_RELOC_RETIRING)

typedef struct brt_reloc_key {
	uint64_t	brk_vdev;
	uint64_t	brk_offset;
	uint64_t	brk_birth;
} brt_reloc_key_t;

typedef struct brt_reloc_entry {
	brt_reloc_key_t	bre_key;
	dva_t		bre_dva;
	uint64_t	bre_birth;
} brt_reloc_entry_t;

typedef struct brt_reloc_multi {
	brt_reloc_entry_t brm_entry;
	dva_t		brm_dvas[SPA_DVAS_PER_BP - 1];
} brt_reloc_multi_t;

typedef struct brt_reloc_retire {
	brt_reloc_key_t	brr_key;
	list_node_t	brr_node;
} brt_reloc_retire_t;

#define	BRT_RELOC_SHARDS	256

typedef struct brt_reloc_shard {
	kmutex_t	brs_lock;
	kcondvar_t	brs_cv;
	list_t		brs_reads;
	const blkptr_t	*brs_draining;
	uint64_t	brs_pending;
} brt_reloc_shard_t;

static int
brt_reloc_compare(const void *a, const void *b)
{
	const brt_reloc_key_t *x = a;
	const brt_reloc_key_t *y = b;
	int cmp = TREE_CMP(x->brk_vdev, y->brk_vdev);
	if (cmp == 0)
		cmp = TREE_CMP(x->brk_offset & ~BRT_RELOC_FLAGS,
		    y->brk_offset & ~BRT_RELOC_FLAGS);
	if (cmp == 0)
		cmp = TREE_CMP(x->brk_birth, y->brk_birth);
	return (cmp);
}

static brt_reloc_key_t
brt_reloc_key(const blkptr_t *bp)
{
	brt_reloc_key_t key = {
		.brk_vdev = DVA_GET_VDEV(&bp->blk_dva[0]),
		.brk_offset = DVA_GET_OFFSET(&bp->blk_dva[0]),
		.brk_birth = BP_GET_PHYSICAL_BIRTH(bp),
	};
	return (key);
}

static brt_reloc_entry_t *
brt_reloc_find(spa_t *spa, const brt_reloc_key_t *key)
{
	brt_reloc_entry_t *entry = zfs_btree_find(&spa->spa_brt_reloc, key,
	    NULL);
	if (entry == NULL)
		entry = zfs_btree_find(&spa->spa_brt_reloc_multi, key, NULL);
	return (entry);
}

static int
brt_reloc_insert(spa_t *spa, const brt_reloc_entry_t *entry, const dva_t *dvas)
{
	brt_reloc_multi_t record = { .brm_entry = *entry };
	zfs_btree_t *tree = &spa->spa_brt_reloc;
	zfs_btree_t *other = &spa->spa_brt_reloc_multi;

	if (DVA_IS_VALID(&dvas[1])) {
		record.brm_entry.bre_key.brk_offset |= BRT_RELOC_EXTRA;
		memcpy(record.brm_dvas, &dvas[1], sizeof (record.brm_dvas));
		tree = &spa->spa_brt_reloc_multi;
		other = &spa->spa_brt_reloc;
	}

	zfs_btree_index_t where;
	if (zfs_btree_find(tree, &entry->bre_key, &where) != NULL ||
	    zfs_btree_find(other, &entry->bre_key, NULL) != NULL)
		return (SET_ERROR(ECKSUM));

	zfs_btree_add_idx_unverified(tree, &record, &where);
	return (0);
}

static const dva_t *
brt_reloc_extra_dvas(const brt_reloc_entry_t *entry)
{
	if (!(entry->bre_key.brk_offset & BRT_RELOC_EXTRA))
		return (NULL);
	return (((const brt_reloc_multi_t *)entry)->brm_dvas);
}

static void
brt_reloc_get_dvas(const brt_reloc_entry_t *entry, dva_t *dvas)
{
	dvas[0] = entry->bre_dva;
	const dva_t *extra = brt_reloc_extra_dvas(entry);
	size_t len = sizeof (dva_t) * (SPA_DVAS_PER_BP - 1);
	if (extra != NULL)
		memcpy(&dvas[1], extra, len);
	else
		memset(&dvas[1], 0, len);
}

static void
brt_reloc_apply(const brt_reloc_entry_t *entry, const blkptr_t *bp,
    blkptr_t *physical)
{
	*physical = *bp;
	physical->blk_dva[0] = entry->bre_dva;
	if (BP_GET_NDVAS(bp) > 1) {
		const dva_t *extra = brt_reloc_extra_dvas(entry);
		for (int i = 1; extra != NULL && i < BP_GET_NDVAS(bp); i++) {
			if (DVA_IS_VALID(&extra[i - 1]))
				physical->blk_dva[i] = extra[i - 1];
		}
	}
	BP_SET_BIRTH(physical, BP_GET_LOGICAL_BIRTH(bp), entry->bre_birth);
}

boolean_t
brt_reloc_eligible(const blkptr_t *bp)
{
	return (bp != NULL &&
	    (BP_IS_METADATA(bp) ||
	    BP_GET_TYPE(bp) == DMU_OT_PLAIN_FILE_CONTENTS ||
	    BP_GET_TYPE(bp) == DMU_OT_ZVOL ||
	    BP_GET_TYPE(bp) == DMU_OT_DEDUP) &&
	    !BP_IS_EMBEDDED(bp) && !BP_IS_HOLE(bp) &&
	    !BP_SHOULD_BYTESWAP(bp) &&
	    !BP_GET_REWRITE(bp) &&
	    BP_COUNT_GANG(bp) == 0);
}

static brt_reloc_shard_t *
brt_reloc_shard(spa_t *spa, const blkptr_t *bp)
{
	const dva_t *dva = &bp->blk_dva[0];
	uint64_t hash = cityhash4(DVA_GET_VDEV(dva), DVA_GET_OFFSET(dva),
	    BP_GET_PHYSICAL_BIRTH(bp), 0);

	return (&spa->spa_brt_reloc_io[hash & (BRT_RELOC_SHARDS - 1)]);
}

static boolean_t
brt_reloc_same_placement(const blkptr_t *a, const blkptr_t *b)
{
	return (DVA_GET_VDEV(&a->blk_dva[0]) == DVA_GET_VDEV(&b->blk_dva[0]) &&
	    DVA_GET_OFFSET(&a->blk_dva[0]) == DVA_GET_OFFSET(&b->blk_dva[0]) &&
	    BP_GET_PHYSICAL_BIRTH(a) == BP_GET_PHYSICAL_BIRTH(b));
}

/*
 * A read chooses and pins its physical placement while holding the map
 * lock. Even unmapped special-vdev reads participate: a move might
 * publish the first entry after one of them has reached the device.
 */
int
brt_reloc_read_start(zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	const blkptr_t *bp = zio->io_bp;

	if (spa->spa_brt_reloc_io == NULL || bp == NULL ||
	    BP_IS_EMBEDDED(bp) || BP_IS_HOLE(bp) ||
	    !spa_feature_is_enabled(spa, SPA_FEATURE_BRT_RELOCATION))
		return (0);

	vdev_t *vd = vdev_lookup_top(spa, DVA_GET_VDEV(&bp->blk_dva[0]));
	boolean_t special = vd != NULL && vd->vdev_mg != NULL &&
	    vd->vdev_mg->mg_class == spa_special_class(spa);
	boolean_t indirect = vd != NULL && vd->vdev_ops == &vdev_indirect_ops;
	boolean_t scan = (zio->io_flags & ZIO_FLAG_SCAN_THREAD) != 0;
	if (!special && ((!indirect && !scan) ||
	    atomic_load_64(&spa->spa_brt_reloc_count) == 0))
		return (0);

	ASSERT3P(bp, ==, &zio->io_bp_copy);
	ASSERT0P(zio->io_brt_shard);
	rw_enter(&spa->spa_brt_reloc_lock, RW_READER);
	/* Removal preserves the original vdev ID in the indirect vdev. */
	if ((special || indirect) &&
	    atomic_load_64(&spa->spa_brt_reloc_count) != 0) {
		brt_reloc_key_t key = brt_reloc_key(bp);
		brt_reloc_entry_t *entry = brt_reloc_find(spa, &key);
		if (entry != NULL) {
			if (entry->bre_key.brk_offset & BRT_RELOC_RETIRING) {
				rw_exit(&spa->spa_brt_reloc_lock);
				return (SET_ERROR(ENOENT));
			}
			brt_reloc_apply(entry, bp, &zio->io_bp_copy);
			zio->io_brt_resolved = B_TRUE;
			zio->io_txg = BP_GET_PHYSICAL_BIRTH(bp);
		}
	}

	if (!special && !scan && !zio->io_brt_resolved) {
		rw_exit(&spa->spa_brt_reloc_lock);
		return (0);
	}

	/*
	 * Sorted scans already carry the resolved DVA. Scan I/O is joined
	 * in the sync thread before the placement can be retired.
	 */
	brt_reloc_shard_t *shard = brt_reloc_shard(spa, bp);
	mutex_enter(&shard->brs_lock);
	list_insert_tail(&shard->brs_reads, zio);
	zio->io_brt_shard = shard;
	mutex_exit(&shard->brs_lock);
	rw_exit(&spa->spa_brt_reloc_lock);
	return (0);
}

void
brt_reloc_read_done(zio_t *zio)
{
	brt_reloc_shard_t *shard = zio->io_brt_shard;
	if (shard == NULL)
		return;

	mutex_enter(&shard->brs_lock);
	list_remove(&shard->brs_reads, zio);
	zio->io_brt_shard = NULL;
	if (shard->brs_draining != NULL &&
	    brt_reloc_same_placement(zio->io_bp, shard->brs_draining)) {
		ASSERT3U(shard->brs_pending, >, 0);
		if (--shard->brs_pending == 0)
			cv_signal(&shard->brs_cv);
	}
	mutex_exit(&shard->brs_lock);
}

/*
 * The sync thread calls this after closing the old placement to new
 * readers. Reads in the same shard can continue to issue and complete.
 */
static void
brt_reloc_wait(spa_t *spa, const blkptr_t *bp)
{
	brt_reloc_shard_t *shard = brt_reloc_shard(spa, bp);
	mutex_enter(&shard->brs_lock);
	ASSERT0P(shard->brs_draining);
	ASSERT0(shard->brs_pending);
	shard->brs_draining = bp;
	for (zio_t *zio = list_head(&shard->brs_reads); zio != NULL;
	    zio = list_next(&shard->brs_reads, zio)) {
		if (brt_reloc_same_placement(zio->io_bp, bp))
			shard->brs_pending++;
	}
	while (shard->brs_pending != 0)
		cv_wait(&shard->brs_cv, &shard->brs_lock);
	shard->brs_draining = NULL;
	mutex_exit(&shard->brs_lock);
}

boolean_t
brt_reloc_resolve(spa_t *spa, const blkptr_t *bp, blkptr_t *physical)
{
	if (likely(atomic_load_64(&spa->spa_brt_reloc_count) == 0) ||
	    bp == NULL || BP_IS_EMBEDDED(bp) || BP_IS_HOLE(bp))
		return (B_FALSE);

	brt_reloc_key_t key = brt_reloc_key(bp);
	rw_enter(&spa->spa_brt_reloc_lock, RW_READER);
	brt_reloc_entry_t *entry = brt_reloc_find(spa, &key);
	if (entry != NULL && physical != NULL)
		brt_reloc_apply(entry, bp, physical);
	rw_exit(&spa->spa_brt_reloc_lock);
	return (entry != NULL);
}

boolean_t
brt_reloc_retire(spa_t *spa, const blkptr_t *bp)
{
	if (likely(atomic_load_64(&spa->spa_brt_reloc_count) == 0))
		return (B_FALSE);

	brt_reloc_key_t key = brt_reloc_key(bp);
	rw_enter(&spa->spa_brt_reloc_lock, RW_WRITER);
	brt_reloc_entry_t *entry = brt_reloc_find(spa, &key);
	boolean_t found = entry != NULL;
	if (found && !(entry->bre_key.brk_offset & BRT_RELOC_QUEUED)) {
		entry->bre_key.brk_offset |= BRT_RELOC_QUEUED;
		/* Flat DDT entries may gain DVAs after a move. */
		if (BP_GET_NDVAS(bp) > 1) {
			dva_t dvas[SPA_DVAS_PER_BP];
			brt_reloc_get_dvas(entry, dvas);
			for (int i = 1; i < BP_GET_NDVAS(bp); i++) {
				if (!DVA_IS_VALID(&dvas[i]))
					dvas[i] = bp->blk_dva[i];
			}
			if (entry->bre_key.brk_offset & BRT_RELOC_EXTRA) {
				brt_reloc_multi_t *record =
				    (brt_reloc_multi_t *)entry;
				memcpy(record->brm_dvas, &dvas[1],
				    sizeof (record->brm_dvas));
			} else if (DVA_IS_VALID(&dvas[1])) {
				brt_reloc_entry_t copy = *entry;
				zfs_btree_remove(&spa->spa_brt_reloc, &key);
				VERIFY0(brt_reloc_insert(spa, &copy, dvas));
				zfs_btree_verify(&spa->spa_brt_reloc_multi);
			}
		}
		brt_reloc_retire_t *retire = kmem_alloc(sizeof (*retire),
		    KM_SLEEP);
		retire->brr_key = key;
		list_link_init(&retire->brr_node);
		list_insert_tail(&spa->spa_brt_retire, retire);
	}
	rw_exit(&spa->spa_brt_reloc_lock);
	return (found);
}

static int brt_entry_refcount(spa_t *, const blkptr_t *, uint64_t *);

static void
brt_reloc_map_create(spa_t *spa, dmu_tx_t *tx)
{
	ASSERT0(spa->spa_brt_reloc_obj);
	rw_enter(&spa->spa_brt_reloc_obj_lock, RW_WRITER);
	spa->spa_brt_reloc_obj = zap_create_flags(spa->spa_meta_objset, 0,
	    ZAP_FLAG_HASH64 | ZAP_FLAG_UINT64_KEY, DMU_OTN_ZAP_METADATA,
	    brt_zap_default_bs, brt_zap_default_ibs, DMU_OT_NONE, 0, tx);
	VERIFY3U(spa->spa_brt_reloc_obj, !=, 0);
	VERIFY0(zap_add(spa->spa_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    BRT_RELOC_MAP_NAME, sizeof (uint64_t), 1,
	    &spa->spa_brt_reloc_obj, tx));
	rw_exit(&spa->spa_brt_reloc_obj_lock);
}

static void
brt_reloc_map_ensure(spa_t *spa, dmu_tx_t *tx)
{
	if (spa->spa_brt_reloc_obj == 0)
		brt_reloc_map_create(spa, tx);
}

static void
brt_reloc_map_destroy(spa_t *spa, dmu_tx_t *tx)
{
	ASSERT0(spa->spa_brt_reloc_count);
	uint64_t object = spa->spa_brt_reloc_obj;
	VERIFY3U(object, !=, 0);
	rw_enter(&spa->spa_brt_reloc_obj_lock, RW_WRITER);
	VERIFY0(zap_remove(spa->spa_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    BRT_RELOC_MAP_NAME, tx));
	VERIFY0(zap_destroy(spa->spa_meta_objset, object, tx));
	spa->spa_brt_reloc_obj = 0;
	rw_exit(&spa->spa_brt_reloc_obj_lock);
}

static void
brt_reloc_zap_key(const brt_reloc_key_t *key,
    uint64_t words[BRT_RELOC_KEY_WORDS])
{
	words[0] = key->brk_vdev;
	words[1] = key->brk_offset & ~BRT_RELOC_FLAGS;
	words[2] = key->brk_birth;
}

static void
brt_reloc_map_remove(spa_t *spa, const brt_reloc_key_t *key, dmu_tx_t *tx)
{
	uint64_t words[BRT_RELOC_KEY_WORDS];
	brt_reloc_zap_key(key, words);
	VERIFY0(zap_remove_uint64(spa->spa_meta_objset,
	    spa->spa_brt_reloc_obj, words, BRT_RELOC_KEY_WORDS, tx));
}

static int
brt_reloc_store(spa_t *spa, const brt_reloc_entry_t *entry, const dva_t *dvas,
    uint64_t refs, boolean_t add, dmu_tx_t *tx)
{
	uint64_t key[BRT_RELOC_KEY_WORDS];
	brt_reloc_zap_key(&entry->bre_key, key);
	uint64_t value[BRT_RELOC_WORDS] = { refs,
	    dvas[0].dva_word[0], dvas[0].dva_word[1], entry->bre_birth,
	    dvas[1].dva_word[0], dvas[1].dva_word[1],
	    dvas[2].dva_word[0], dvas[2].dva_word[1] };
	if (add) {
		return (zap_add_uint64(spa->spa_meta_objset,
		    spa->spa_brt_reloc_obj, key, BRT_RELOC_KEY_WORDS,
		    sizeof (uint64_t), BRT_RELOC_WORDS, value, tx));
	}
	return (zap_update_uint64(spa->spa_meta_objset,
	    spa->spa_brt_reloc_obj, key, BRT_RELOC_KEY_WORDS,
	    sizeof (uint64_t), BRT_RELOC_WORDS, value, tx));
}

int
brt_relocate(spa_t *spa, const blkptr_t *source, const blkptr_t *dest,
    dmu_tx_t *tx)
{
	ASSERT(dmu_tx_is_syncing(tx));

	if (brt_reloc_resolve(spa, source, NULL))
		return (SET_ERROR(EALREADY));
	if (atomic_load_64(&spa->spa_brt_reloc_count) == 0) {
		spa_config_enter(spa, SCL_ZIO, FTAG, RW_WRITER);
		spa_config_exit(spa, SCL_ZIO, FTAG);
	}

	brt_reloc_entry_t entry = {
		.bre_key = brt_reloc_key(source),
		.bre_dva = dest->blk_dva[0],
		.bre_birth = BP_GET_PHYSICAL_BIRTH(dest),
	};
	dva_t dvas[SPA_DVAS_PER_BP] = { 0 };
	for (int i = 0; i < BP_GET_NDVAS(dest); i++)
		dvas[i] = dest->blk_dva[i];

	brt_vdev_t *brtvd = brt_vdev(spa, entry.bre_key.brk_vdev, B_TRUE);
	if (!brtvd->bv_initiated) {
		rw_enter(&brtvd->bv_lock, RW_WRITER);
		brt_vdev_realloc(spa, brtvd);
		rw_exit(&brtvd->bv_lock);
	}
	uint64_t refs;
	int error = brt_entry_refcount(spa, source, &refs);
	if (error == 0 && brtvd->bv_mos_entries == 0) {
		brt_vdev_create(spa, brtvd, tx);
		brtvd->bv_meta_dirty = B_TRUE;
	}
	if (error == 0)
		brt_reloc_map_ensure(spa, tx);
	if (error == 0) {
		error = brt_reloc_store(spa, &entry, dvas, refs, B_TRUE, tx);
	}
	if (error != 0) {
		if (spa->spa_brt_reloc_count == 0 &&
		    spa->spa_brt_reloc_obj != 0)
			brt_reloc_map_destroy(spa, tx);
		return (error);
	}

	if (brtvd->bv_mos_entries != 0) {
		error = zap_remove_uint64_by_dnode(brtvd->bv_mos_entries_dnode,
		    &entry.bre_key.brk_offset, BRT_KEY_WORDS, tx);
		VERIFY(error == 0 || error == ENOENT);
	}
	if (atomic_load_64(&spa->spa_brt_reloc_count) == 0)
		spa_feature_incr(spa, SPA_FEATURE_BRT_RELOCATION, tx);
	brtvd->bv_reloc_count++;

	rw_enter(&spa->spa_brt_reloc_lock, RW_WRITER);
	VERIFY0(brt_reloc_insert(spa, &entry, dvas));
	zfs_btree_verify(&spa->spa_brt_reloc);
	zfs_btree_verify(&spa->spa_brt_reloc_multi);
	atomic_inc_64(&spa->spa_brt_reloc_count);
	rw_exit(&spa->spa_brt_reloc_lock);

	if (refs > 0) {
		uint64_t idx = DVA_GET_OFFSET(&source->blk_dva[0]) /
		    spa->spa_brt_rangesize;
		ASSERT3U(idx, <, brtvd->bv_size);
		rw_enter(&brtvd->bv_lock, RW_WRITER);
		brt_vdev_entcount_dec(brtvd, idx);
		brtvd->bv_meta_dirty = TRUE;
		brtvd->bv_entcount_dirty = TRUE;
		BT_SET(brtvd->bv_bitmap,
		    idx / (BRT_BLOCKSIZE / sizeof (uint16_t)));
		rw_exit(&brtvd->bv_lock);
	}

	brt_reloc_wait(spa, source);
	metaslab_free(spa, source, dmu_tx_get_txg(tx), B_FALSE);
	return (0);
}

static void
brt_reloc_sync(spa_t *spa, dmu_tx_t *tx)
{
	for (;;) {
		rw_enter(&spa->spa_brt_reloc_lock, RW_WRITER);
		brt_reloc_retire_t *retire = list_remove_head(
		    &spa->spa_brt_retire);
		if (retire == NULL) {
			rw_exit(&spa->spa_brt_reloc_lock);
			break;
		}
		brt_reloc_key_t key = retire->brr_key;
		brt_reloc_entry_t *entry = brt_reloc_find(spa, &key);
		VERIFY3P(entry, !=, NULL);
		entry->bre_key.brk_offset |= BRT_RELOC_RETIRING;
		boolean_t extra = (entry->bre_key.brk_offset & BRT_RELOC_EXTRA)
		    != 0;

		blkptr_t physical = { 0 };
		brt_reloc_get_dvas(entry, physical.blk_dva);
		BP_SET_BIRTH(&physical, entry->bre_birth, 0);
		rw_exit(&spa->spa_brt_reloc_lock);
		kmem_free(retire, sizeof (*retire));

		brt_vdev_t *brtvd = brt_vdev(spa, key.brk_vdev, B_FALSE);
		brt_reloc_map_remove(spa, &key, tx);
		brt_reloc_wait(spa, &physical);

		rw_enter(&spa->spa_brt_reloc_lock, RW_WRITER);
		zfs_btree_remove(extra ? &spa->spa_brt_reloc_multi :
		    &spa->spa_brt_reloc, &key);
		atomic_dec_64(&spa->spa_brt_reloc_count);
		rw_exit(&spa->spa_brt_reloc_lock);

		metaslab_free(spa, &physical, dmu_tx_get_txg(tx), B_FALSE);

		if (--brtvd->bv_reloc_count == 0 && brtvd->bv_totalcount == 0)
			brt_vdev_destroy(spa, brtvd, tx);
		if (atomic_load_64(&spa->spa_brt_reloc_count) == 0) {
			brt_reloc_map_destroy(spa, tx);
			spa_feature_decr(spa, SPA_FEATURE_BRT_RELOCATION, tx);
		}
	}
}

static int
brt_reloc_entry_load(spa_t *spa,
    const uint64_t key[BRT_RELOC_KEY_WORDS],
    const uint64_t value[BRT_RELOC_WORDS])
{
	dva_t physical[SPA_DVAS_PER_BP] = {
		{ { value[1], value[2] } },
		{ { value[4], value[5] } },
		{ { value[6], value[7] } },
	};
	if (key[0] >= spa->spa_brt_nvdevs ||
	    !IS_P2ALIGNED(key[1], SPA_MINBLOCKSIZE) ||
	    !DVA_IS_VALID(&physical[0]) ||
	    (DVA_IS_VALID(&physical[2]) && !DVA_IS_VALID(&physical[1])) ||
	    value[3] <= key[2])
		return (SET_ERROR(ECKSUM));

	for (int i = 0; i < SPA_DVAS_PER_BP; i++) {
		if (!DVA_IS_VALID(&physical[i]))
			continue;
		if (DVA_GET_GANG(&physical[i]) ||
		    DVA_GET_VDEV(&physical[i]) >=
		    spa->spa_root_vdev->vdev_children)
			return (SET_ERROR(ECKSUM));
	}

	brt_vdev_t *brtvd = spa->spa_brt_vdevs[key[0]];
	if (brtvd->bv_mos_entries == 0)
		return (SET_ERROR(ECKSUM));
	if (!brtvd->bv_initiated) {
		rw_enter(&brtvd->bv_lock, RW_WRITER);
		brt_vdev_realloc(spa, brtvd);
		rw_exit(&brtvd->bv_lock);
	}
	brt_reloc_entry_t entry = {
		.bre_key = { key[0], key[1], key[2] },
		.bre_dva = physical[0],
		.bre_birth = value[3],
	};
	int error = brt_reloc_insert(spa, &entry, physical);
	if (error == 0)
		brtvd->bv_reloc_count++;
	return (error);
}

static int
brt_reloc_map_load(spa_t *spa)
{
	zap_cursor_t zc;
	uint64_t key[BRT_RELOC_KEY_WORDS];
	uint64_t value[BRT_RELOC_WORDS];
	int error;

	for (zap_cursor_init(&zc, spa->spa_meta_objset,
	    spa->spa_brt_reloc_obj);
	    (error = zap_cursor_retrieve_uint64(&zc, BRT_RELOC_KEY_WORDS,
	    key, BRT_RELOC_WORDS, value)) == 0; zap_cursor_advance(&zc)) {
		error = brt_reloc_entry_load(spa, key, value);
		if (error != 0)
			break;
	}
	zap_cursor_fini(&zc);
	if (error != ENOENT)
		return (error);

	/* zdb can enable a full-tree check. Run it once after loading. */
	zfs_btree_verify(&spa->spa_brt_reloc);
	zfs_btree_verify(&spa->spa_brt_reloc_multi);
	return (0);
}

static int
brt_entry_lookup(spa_t *spa, brt_vdev_t *brtvd, brt_entry_t *bre)
{
	uint64_t off = BRE_OFFSET(bre);

	if (brtvd->bv_reloc_count != 0 &&
	    brt_reloc_resolve(spa, &bre->bre_bp, NULL)) {
		brt_reloc_key_t key = brt_reloc_key(&bre->bre_bp);
		uint64_t words[BRT_RELOC_KEY_WORDS];
		uint64_t value[BRT_RELOC_WORDS] = { 0 };
		brt_reloc_zap_key(&key, words);
		int error = zap_lookup_uint64(spa->spa_meta_objset,
		    spa->spa_brt_reloc_obj, words, BRT_RELOC_KEY_WORDS,
		    sizeof (uint64_t), BRT_RELOC_WORDS, value);
		if (error == 0)
			bre->bre_count = value[0];
		return (error);
	}

	if (brtvd->bv_mos_entries == 0)
		return (SET_ERROR(ENOENT));

	if (brt_has_endian_fixed(spa)) {
		return (zap_lookup_uint64_by_dnode(brtvd->bv_mos_entries_dnode,
		    &off, BRT_KEY_WORDS, sizeof (bre->bre_count), 1,
		    &bre->bre_count));
	} else {
		return (zap_lookup_uint64_by_dnode(brtvd->bv_mos_entries_dnode,
		    &off, BRT_KEY_WORDS, 1, sizeof (bre->bre_count),
		    &bre->bre_count));
	}
}

/*
 * Return TRUE if we _can_ have BRT entry for this bp. It might be false
 * positive, but gives us quick answer if we should look into BRT, which
 * may require reads and thus will be more expensive.
 */
boolean_t
brt_maybe_exists(spa_t *spa, const blkptr_t *bp)
{
	if (brt_reloc_resolve(spa, bp, NULL))
		return (B_TRUE);

	if (spa->spa_brt_nvdevs == 0)
		return (B_FALSE);

	uint64_t vdevid = DVA_GET_VDEV(&bp->blk_dva[0]);
	brt_vdev_t *brtvd = brt_vdev(spa, vdevid, B_FALSE);
	if (brtvd == NULL || !brtvd->bv_initiated)
		return (FALSE);

	/*
	 * We don't need locks here, since bv_entcount pointer must be
	 * stable at this point, and we don't care about false positive
	 * races here, while false negative should be impossible, since
	 * all brt_vdev_addref() have already completed by this point.
	 */
	uint64_t off = DVA_GET_OFFSET(&bp->blk_dva[0]);
	return (brt_vdev_lookup(spa, brtvd, off));
}

/*
 * Estimate the worst-case amount of MOS data the sync thread may dirty
 * to add, update or remove one BRT entry: one ZAP leaf block.  Unlike
 * the DDT, BRT ZAPs do not use prehashed keys, so even consecutive
 * offsets scatter across leaves and rarely combine.
 */
uint64_t
brt_sync_dirty_est(spa_t *spa)
{
	(void) spa;
	return (1ULL << brt_zap_default_bs);
}

uint64_t
brt_get_dspace(spa_t *spa)
{
	if (spa->spa_brt_nvdevs == 0)
		return (0);

	brt_rlock(spa);
	uint64_t s = 0;
	for (uint64_t vdevid = 0; vdevid < spa->spa_brt_nvdevs; vdevid++)
		s += spa->spa_brt_vdevs[vdevid]->bv_savedspace;
	brt_unlock(spa);
	return (s);
}

uint64_t
brt_get_used(spa_t *spa)
{
	if (spa->spa_brt_nvdevs == 0)
		return (0);

	brt_rlock(spa);
	uint64_t s = 0;
	for (uint64_t vdevid = 0; vdevid < spa->spa_brt_nvdevs; vdevid++)
		s += spa->spa_brt_vdevs[vdevid]->bv_usedspace;
	brt_unlock(spa);
	return (s);
}

uint64_t
brt_get_saved(spa_t *spa)
{
	return (brt_get_dspace(spa));
}

uint64_t
brt_get_ratio(spa_t *spa)
{
	uint64_t used = brt_get_used(spa);
	if (used == 0)
		return (100);
	return ((used + brt_get_saved(spa)) * 100 / used);
}

static int
brt_kstats_update(kstat_t *ksp, int rw)
{
	brt_stats_t *bs = ksp->ks_data;

	if (rw == KSTAT_WRITE)
		return (EACCES);

	bs->brt_addref_entry_not_on_disk.value.ui64 =
	    wmsum_value(&brt_sums.brt_addref_entry_not_on_disk);
	bs->brt_addref_entry_on_disk.value.ui64 =
	    wmsum_value(&brt_sums.brt_addref_entry_on_disk);
	bs->brt_decref_entry_in_memory.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_entry_in_memory);
	bs->brt_decref_entry_loaded_from_disk.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_entry_loaded_from_disk);
	bs->brt_decref_entry_not_in_memory.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_entry_not_in_memory);
	bs->brt_decref_entry_read_lost_race.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_entry_read_lost_race);
	bs->brt_decref_entry_still_referenced.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_entry_still_referenced);
	bs->brt_decref_free_data_later.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_free_data_later);
	bs->brt_decref_free_data_now.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_free_data_now);
	bs->brt_decref_no_entry.value.ui64 =
	    wmsum_value(&brt_sums.brt_decref_no_entry);

	return (0);
}

static void
brt_stat_init(void)
{

	wmsum_init(&brt_sums.brt_addref_entry_not_on_disk, 0);
	wmsum_init(&brt_sums.brt_addref_entry_on_disk, 0);
	wmsum_init(&brt_sums.brt_decref_entry_in_memory, 0);
	wmsum_init(&brt_sums.brt_decref_entry_loaded_from_disk, 0);
	wmsum_init(&brt_sums.brt_decref_entry_not_in_memory, 0);
	wmsum_init(&brt_sums.brt_decref_entry_read_lost_race, 0);
	wmsum_init(&brt_sums.brt_decref_entry_still_referenced, 0);
	wmsum_init(&brt_sums.brt_decref_free_data_later, 0);
	wmsum_init(&brt_sums.brt_decref_free_data_now, 0);
	wmsum_init(&brt_sums.brt_decref_no_entry, 0);

	brt_ksp = kstat_create("zfs", 0, "brtstats", "misc", KSTAT_TYPE_NAMED,
	    sizeof (brt_stats) / sizeof (kstat_named_t), KSTAT_FLAG_VIRTUAL);
	if (brt_ksp != NULL) {
		brt_ksp->ks_data = &brt_stats;
		brt_ksp->ks_update = brt_kstats_update;
		kstat_install(brt_ksp);
	}
}

static void
brt_stat_fini(void)
{
	if (brt_ksp != NULL) {
		kstat_delete(brt_ksp);
		brt_ksp = NULL;
	}

	wmsum_fini(&brt_sums.brt_addref_entry_not_on_disk);
	wmsum_fini(&brt_sums.brt_addref_entry_on_disk);
	wmsum_fini(&brt_sums.brt_decref_entry_in_memory);
	wmsum_fini(&brt_sums.brt_decref_entry_loaded_from_disk);
	wmsum_fini(&brt_sums.brt_decref_entry_not_in_memory);
	wmsum_fini(&brt_sums.brt_decref_entry_read_lost_race);
	wmsum_fini(&brt_sums.brt_decref_entry_still_referenced);
	wmsum_fini(&brt_sums.brt_decref_free_data_later);
	wmsum_fini(&brt_sums.brt_decref_free_data_now);
	wmsum_fini(&brt_sums.brt_decref_no_entry);
}

void
brt_init(void)
{
	brt_entry_cache = kmem_cache_create("brt_entry_cache",
	    sizeof (brt_entry_t), 0, NULL, NULL, NULL, NULL, NULL, 0);

	brt_stat_init();
}

void
brt_fini(void)
{
	brt_stat_fini();

	kmem_cache_destroy(brt_entry_cache);
}

/* Return TRUE if block should be freed immediately. */
boolean_t
brt_entry_decref(spa_t *spa, const blkptr_t *bp)
{
	brt_entry_t *bre, *racebre;
	brt_entry_t bre_search;
	avl_index_t where;
	uint64_t vdevid;
	int error;
	boolean_t relocated = brt_reloc_resolve(spa, bp, NULL);

	brt_entry_fill(bp, &bre_search, &vdevid);

	brt_vdev_t *brtvd = brt_vdev(spa, vdevid, B_FALSE);
	ASSERT(brtvd != NULL);

	rw_enter(&brtvd->bv_lock, RW_WRITER);
	ASSERT(brtvd->bv_initiated);
	bre = avl_find(&brtvd->bv_tree, &bre_search, NULL);
	if (bre != NULL) {
		BRTSTAT_BUMP(brt_decref_entry_in_memory);
		goto out;
	} else {
		BRTSTAT_BUMP(brt_decref_entry_not_in_memory);
	}
	rw_exit(&brtvd->bv_lock);

	error = brt_entry_lookup(spa, brtvd, &bre_search);
	/* bre_search now contains correct bre_count */
	if (error == ENOENT) {
		BRTSTAT_BUMP(brt_decref_no_entry);
		return (B_TRUE);
	}
	ASSERT0(error);

	rw_enter(&brtvd->bv_lock, RW_WRITER);
	racebre = avl_find(&brtvd->bv_tree, &bre_search, &where);
	if (racebre != NULL) {
		/* The entry was added when the lock was dropped. */
		BRTSTAT_BUMP(brt_decref_entry_read_lost_race);
		bre = racebre;
		goto out;
	}

	BRTSTAT_BUMP(brt_decref_entry_loaded_from_disk);
	/*
	 * A persisted relocation can have zero extra references. Its last
	 * free retires the mapping without changing the BRT counters.
	 */
	if (bre_search.bre_count == 0) {
		rw_exit(&brtvd->bv_lock);
		BRTSTAT_BUMP(brt_decref_free_data_now);
		return (B_TRUE);
	}
	bre = kmem_cache_alloc(brt_entry_cache, KM_SLEEP);
	bre->bre_bp = bre_search.bre_bp;
	bre->bre_count = bre_search.bre_count;
	bre->bre_pcount = 0;
	avl_insert(&brtvd->bv_tree, bre, where);

out:
	if (bre->bre_count == 0) {
		rw_exit(&brtvd->bv_lock);
		BRTSTAT_BUMP(brt_decref_free_data_now);
		return (B_TRUE);
	}

	bre->bre_pcount--;
	ASSERT(bre->bre_count > 0);
	bre->bre_count--;
	if (bre->bre_count == 0)
		BRTSTAT_BUMP(brt_decref_free_data_later);
	else
		BRTSTAT_BUMP(brt_decref_entry_still_referenced);
	brt_vdev_decref(spa, brtvd, bre, bp_get_dsize_sync(spa, bp), relocated);

	rw_exit(&brtvd->bv_lock);

	return (B_FALSE);
}

static int
brt_entry_refcount(spa_t *spa, const blkptr_t *bp, uint64_t *refcnt)
{
	brt_entry_t search;
	uint64_t vdevid;

	brt_entry_fill(bp, &search, &vdevid);
	brt_vdev_t *brtvd = brt_vdev(spa, vdevid, B_FALSE);
	ASSERT(brtvd != NULL);

	rw_enter(&brtvd->bv_lock, RW_READER);
	ASSERT(brtvd->bv_initiated);
	brt_entry_t *bre = avl_find(&brtvd->bv_tree, &search, NULL);
	if (bre != NULL) {
		*refcnt = bre->bre_count;
		rw_exit(&brtvd->bv_lock);
		return (0);
	}
	rw_exit(&brtvd->bv_lock);

	int error = brt_entry_lookup(spa, brtvd, &search);
	if (error != 0 && error != ENOENT)
		return (error);

	*refcnt = search.bre_count;
	return (0);
}

uint64_t
brt_entry_get_refcount(spa_t *spa, const blkptr_t *bp)
{
	uint64_t refcnt = 0;
	int error __maybe_unused = brt_entry_refcount(spa, bp, &refcnt);
	ASSERT0(error);
	return (refcnt);
}

static void
brt_prefetch(brt_vdev_t *brtvd, const blkptr_t *bp)
{
	if (!brt_zap_prefetch)
		return;
	if (brtvd->bv_mos_entries == 0)
		return;

	uint64_t off = DVA_GET_OFFSET(&bp->blk_dva[0]);
	rw_enter(&brtvd->bv_mos_entries_lock, RW_READER);
	if (brtvd->bv_mos_entries != 0) {
		(void) zap_prefetch_uint64_by_dnode(brtvd->bv_mos_entries_dnode,
		    &off, BRT_KEY_WORDS);
	}
	rw_exit(&brtvd->bv_mos_entries_lock);
}

static int
brt_entry_compare(const void *x1, const void *x2)
{
	const brt_entry_t *bre1 = x1, *bre2 = x2;
	const blkptr_t *bp1 = &bre1->bre_bp, *bp2 = &bre2->bre_bp;

	int cmp = TREE_CMP(DVA_GET_OFFSET(&bp1->blk_dva[0]),
	    DVA_GET_OFFSET(&bp2->blk_dva[0]));
	if (cmp == 0)
		cmp = TREE_CMP(BP_GET_PHYSICAL_BIRTH(bp1),
		    BP_GET_PHYSICAL_BIRTH(bp2));
	return (cmp);
}

static int
brt_entry_dedup_compare(const void *x1, const void *x2)
{
	const brt_entry_t *bre1 = x1, *bre2 = x2;
	const blkptr_t *bp1 = &bre1->bre_bp, *bp2 = &bre2->bre_bp;
	const uint64_t *k1 = bp1->blk_cksum.zc_word;
	const uint64_t *k2 = bp2->blk_cksum.zc_word;
	int cmp;

	/* Sort by the checksum, matching the DDT ZAP hash order. */
	for (int i = 0; i < (sizeof (zio_cksum_t) / sizeof (uint64_t)); i++) {
		if (likely((cmp = TREE_CMP(k1[i], k2[i])) != 0))
			return (cmp);
	}

	/*
	 * The same checksum may reference different blocks if the DDT
	 * entry for the older one was pruned.
	 */
	cmp = TREE_CMP(DVA_GET_VDEV(&bp1->blk_dva[0]),
	    DVA_GET_VDEV(&bp2->blk_dva[0]));
	if (likely(cmp == 0)) {
		cmp = TREE_CMP(DVA_GET_OFFSET(&bp1->blk_dva[0]),
		    DVA_GET_OFFSET(&bp2->blk_dva[0]));
	}
	if (cmp == 0) {
		cmp = TREE_CMP(BP_GET_PHYSICAL_BIRTH(bp1),
		    BP_GET_PHYSICAL_BIRTH(bp2));
	}
	return (cmp);
}

void
brt_pending_add(spa_t *spa, const blkptr_t *bp, dmu_tx_t *tx)
{
	brt_entry_t *bre, *newbre;
	avl_index_t where;
	uint64_t txg;

	txg = dmu_tx_get_txg(tx);
	ASSERT3U(txg, !=, 0);

	newbre = kmem_cache_alloc(brt_entry_cache, KM_SLEEP);
	newbre->bre_bp = *bp;
	newbre->bre_count = 0;
	newbre->bre_pcount = 1;

	/*
	 * Blocks with the DEDUP bit set are referenced in the DDT instead
	 * of the BRT and are kept on separate trees until then.
	 */
	if (BP_GET_DEDUP(bp)) {
		brt_dedup_shard_t *bds =
		    &spa->spa_brt_dedup[BRT_DEDUP_SHARD(bp)];
		avl_tree_t *pending_tree = &bds->bds_tree[txg & TXG_MASK];

		mutex_enter(&bds->bds_lock);
		bre = avl_find(pending_tree, newbre, &where);
		if (bre == NULL) {
			avl_insert(pending_tree, newbre, where);
			newbre = NULL;
		} else {
			bre->bre_pcount++;
		}
		mutex_exit(&bds->bds_lock);

		if (newbre != NULL) {
			kmem_cache_free(brt_entry_cache, newbre);
		} else {
			/* Prefetch DDT entry for the syncing context. */
			ddt_prefetch(spa, bp);
		}
		return;
	}

	uint64_t vdevid = DVA_GET_VDEV(&bp->blk_dva[0]);
	brt_vdev_t *brtvd = brt_vdev(spa, vdevid, B_TRUE);
	avl_tree_t *pending_tree = &brtvd->bv_pending_tree[txg & TXG_MASK];

	mutex_enter(&brtvd->bv_pending_lock);
	bre = avl_find(pending_tree, newbre, &where);
	if (bre == NULL) {
		avl_insert(pending_tree, newbre, where);
		newbre = NULL;
	} else {
		bre->bre_pcount++;
	}
	mutex_exit(&brtvd->bv_pending_lock);

	if (newbre != NULL) {
		ASSERT(bre != NULL);
		ASSERT(bre != newbre);
		kmem_cache_free(brt_entry_cache, newbre);
	} else {
		ASSERT0P(bre);

		/* Prefetch BRT entry for the syncing context. */
		brt_prefetch(brtvd, bp);
	}
}

void
brt_pending_remove(spa_t *spa, const blkptr_t *bp, dmu_tx_t *tx)
{
	brt_entry_t *bre, bre_search;
	uint64_t txg;

	txg = dmu_tx_get_txg(tx);
	ASSERT3U(txg, !=, 0);

	bre_search.bre_bp = *bp;

	kmutex_t *pending_lock;
	avl_tree_t *pending_tree;
	if (BP_GET_DEDUP(bp)) {
		brt_dedup_shard_t *bds =
		    &spa->spa_brt_dedup[BRT_DEDUP_SHARD(bp)];
		pending_lock = &bds->bds_lock;
		pending_tree = &bds->bds_tree[txg & TXG_MASK];
	} else {
		uint64_t vdevid = DVA_GET_VDEV(&bp->blk_dva[0]);
		brt_vdev_t *brtvd = brt_vdev(spa, vdevid, B_FALSE);
		ASSERT(brtvd != NULL);
		pending_lock = &brtvd->bv_pending_lock;
		pending_tree = &brtvd->bv_pending_tree[txg & TXG_MASK];
	}

	mutex_enter(pending_lock);
	bre = avl_find(pending_tree, &bre_search, NULL);
	ASSERT(bre != NULL);
	ASSERT(bre->bre_pcount > 0);
	bre->bre_pcount--;
	if (bre->bre_pcount == 0)
		avl_remove(pending_tree, bre);
	else
		bre = NULL;
	mutex_exit(pending_lock);

	if (bre)
		kmem_cache_free(brt_entry_cache, bre);
}

typedef struct brt_pending_vdev_arg {
	spa_t		*bpva_spa;
	brt_vdev_t	*bpva_brtvd;
	uint64_t	bpva_txg;
} brt_pending_vdev_arg_t;

static void
brt_pending_apply_vdev(void *arg)
{
	brt_pending_vdev_arg_t *bpva = arg;
	spa_t *spa = bpva->bpva_spa;
	brt_vdev_t *brtvd = bpva->bpva_brtvd;
	uint64_t txg = bpva->bpva_txg;
	brt_entry_t *bre, *nbre;

	/*
	 * We are in syncing context, so no other bv_pending_tree accesses
	 * are possible for the TXG.  So we don't need bv_pending_lock.
	 */
	ASSERT(avl_is_empty(&brtvd->bv_tree));
	avl_swap(&brtvd->bv_tree, &brtvd->bv_pending_tree[txg & TXG_MASK]);

	for (bre = avl_first(&brtvd->bv_tree); bre; bre = nbre) {
		nbre = AVL_NEXT(&brtvd->bv_tree, bre);

		/*
		 * Unless we know that the block is definitely not in ZAP,
		 * try to get its reference count from there.
		 */
		uint64_t off = BRE_OFFSET(bre);
		if (brtvd->bv_mos_entries != 0 &&
		    (brt_vdev_lookup(spa, brtvd, off) ||
		    brt_reloc_resolve(spa, &bre->bre_bp, NULL))) {
			int error = brt_entry_lookup(spa, brtvd, bre);
			if (error == 0) {
				BRTSTAT_BUMP(brt_addref_entry_on_disk);
			} else {
				ASSERT3U(error, ==, ENOENT);
				BRTSTAT_BUMP(brt_addref_entry_not_on_disk);
			}
		}
	}

	/* If we had no new clones for this vdev, we don't need to initiate. */
	if (avl_is_empty(&brtvd->bv_tree))
		return;

	if (!brtvd->bv_initiated) {
		rw_enter(&brtvd->bv_lock, RW_WRITER);
		brt_vdev_realloc(spa, brtvd);
		rw_exit(&brtvd->bv_lock);
	}

	/*
	 * Convert pending references into proper ones.  This has to be a
	 * separate loop, since entcount modifications would cause false
	 * positives for brt_vdev_lookup() on following iterations.
	 */
	for (bre = avl_first(&brtvd->bv_tree); bre;
	    bre = AVL_NEXT(&brtvd->bv_tree, bre)) {
		brt_vdev_addref(spa, brtvd, bre,
		    bp_get_dsize(spa, &bre->bre_bp), bre->bre_pcount);
		bre->bre_count += bre->bre_pcount;
	}
}

typedef struct brt_pending_dedup_arg {
	spa_t		*bpda_spa;
	avl_tree_t	*bpda_tree;
} brt_pending_dedup_arg_t;

static void
brt_pending_apply_dedup(void *arg)
{
	brt_pending_dedup_arg_t *bpda = arg;
	spa_t *spa = bpda->bpda_spa;
	avl_tree_t *tree = bpda->bpda_tree;
	brt_entry_t *bre, *nbre;

	for (bre = avl_first(tree); bre; bre = nbre) {
		nbre = AVL_NEXT(tree, bre);
		while (bre->bre_pcount > 0) {
			if (!ddt_addref(spa, &bre->bre_bp))
				break;
			bre->bre_pcount--;
		}
		if (bre->bre_pcount == 0) {
			avl_remove(tree, bre);
			kmem_cache_free(brt_entry_cache, bre);
		}
		/* Else leave it for the caller to reference in the BRT. */
	}
}

void
brt_pending_apply(spa_t *spa, uint64_t txg)
{
	brt_pending_dedup_arg_t bpda[BRT_DEDUP_SHARDS];
	taskq_t *tq = spa->spa_dsl_pool->dp_sync_taskq;

	/*
	 * We are in syncing context, so no open context accesses to the
	 * pending trees of this TXG are possible and we need no locks.
	 *
	 * Reference the dedup'd blocks in the DDT.  Process the shards in
	 * parallel, since random DDT ZAP lookups are CPU-expensive due to
	 * the leaf block decompression.  Each shard covers a disjoint part
	 * of the DDT ZAP hash space and is walked in the hash order, so
	 * each leaf is decompressed at most once and only by one thread.
	 */
	for (int i = 0; i < BRT_DEDUP_SHARDS; i++) {
		avl_tree_t *tree =
		    &spa->spa_brt_dedup[i].bds_tree[txg & TXG_MASK];
		if (avl_is_empty(tree))
			continue;
		bpda[i].bpda_spa = spa;
		bpda[i].bpda_tree = tree;
		VERIFY(taskq_dispatch(tq, brt_pending_apply_dedup,
		    &bpda[i], TQ_SLEEP) != TASKQID_INVALID);
	}
	taskq_wait(tq);

	/*
	 * Turn blocks that could not be referenced in the DDT (their
	 * entries were pruned or are over the dedup quota) into regular
	 * BRT pending entries for their vdevs.
	 */
	for (int i = 0; i < BRT_DEDUP_SHARDS; i++) {
		avl_tree_t *tree =
		    &spa->spa_brt_dedup[i].bds_tree[txg & TXG_MASK];
		brt_entry_t *bre;
		void *cookie = NULL;

		while ((bre = avl_destroy_nodes(tree, &cookie)) != NULL) {
			uint64_t vdevid = DVA_GET_VDEV(&bre->bre_bp.blk_dva[0]);
			brt_vdev_t *brtvd = brt_vdev(spa, vdevid, B_TRUE);
			avl_add(&brtvd->bv_pending_tree[txg & TXG_MASK], bre);
		}
	}

	/*
	 * Add pending references to the BRTs of their vdevs.  Process the
	 * vdevs in parallel, since random BRT ZAP lookups are CPU-expensive
	 * due to the leaf block decompression.  The BRT ZAP hash is salted,
	 * so unlike the DDT above the lookups can not be ordered to match
	 * the leaf blocks order.
	 */
	brt_rlock(spa);
	uint64_t nvdevs = spa->spa_brt_nvdevs;
	brt_unlock(spa);
	if (nvdevs == 0)
		return;
	brt_pending_vdev_arg_t *bpva =
	    kmem_zalloc(sizeof (*bpva) * nvdevs, KM_SLEEP);
	brt_rlock(spa);
	for (uint64_t vdevid = 0; vdevid < nvdevs; vdevid++) {
		brt_vdev_t *brtvd = spa->spa_brt_vdevs[vdevid];
		if (avl_is_empty(&brtvd->bv_pending_tree[txg & TXG_MASK]))
			continue;
		bpva[vdevid].bpva_spa = spa;
		bpva[vdevid].bpva_brtvd = brtvd;
		bpva[vdevid].bpva_txg = txg;
	}
	brt_unlock(spa);
	for (uint64_t vdevid = 0; vdevid < nvdevs; vdevid++) {
		if (bpva[vdevid].bpva_spa == NULL)
			continue;
		VERIFY(taskq_dispatch(tq, brt_pending_apply_vdev,
		    &bpva[vdevid], TQ_SLEEP) != TASKQID_INVALID);
	}
	taskq_wait(tq);
	kmem_free(bpva, sizeof (*bpva) * nvdevs);
}

static void
brt_sync_entry(spa_t *spa, dnode_t *dn, brt_entry_t *bre, dmu_tx_t *tx)
{
	uint64_t off = BRE_OFFSET(bre);

	if (bre->bre_pcount != 0 &&
	    atomic_load_64(&spa->spa_brt_reloc_count) != 0) {
		brt_reloc_key_t key = brt_reloc_key(&bre->bre_bp);
		rw_enter(&spa->spa_brt_reloc_lock, RW_READER);
		brt_reloc_entry_t *entry = brt_reloc_find(spa, &key);
		if (entry != NULL) {
			brt_reloc_entry_t copy = *entry;
			dva_t dvas[SPA_DVAS_PER_BP];
			brt_reloc_get_dvas(entry, dvas);
			rw_exit(&spa->spa_brt_reloc_lock);
			VERIFY0(brt_reloc_store(spa, &copy, dvas,
			    bre->bre_count, B_FALSE, tx));
			return;
		}
		rw_exit(&spa->spa_brt_reloc_lock);
	}

	if (bre->bre_pcount == 0) {
		/* The net change is zero, nothing to do in ZAP. */
	} else if (bre->bre_count == 0) {
		int error = zap_remove_uint64_by_dnode(dn, &off,
		    BRT_KEY_WORDS, tx);
		VERIFY(error == 0 || error == ENOENT);
	} else {
		if (brt_has_endian_fixed(spa)) {
			VERIFY0(zap_update_uint64_by_dnode(dn, &off,
			    BRT_KEY_WORDS, sizeof (bre->bre_count), 1,
			    &bre->bre_count, tx));
		} else {
			VERIFY0(zap_update_uint64_by_dnode(dn, &off,
			    BRT_KEY_WORDS, 1, sizeof (bre->bre_count),
			    &bre->bre_count, tx));
		}
	}
}

static void
brt_sync_table(spa_t *spa, dmu_tx_t *tx)
{
	brt_entry_t *bre;

	brt_rlock(spa);
	for (uint64_t vdevid = 0; vdevid < spa->spa_brt_nvdevs; vdevid++) {
		brt_vdev_t *brtvd = spa->spa_brt_vdevs[vdevid];
		brt_unlock(spa);

		if (!brtvd->bv_meta_dirty) {
			ASSERT(!brtvd->bv_entcount_dirty);
			ASSERT0(avl_numnodes(&brtvd->bv_tree));
			brt_rlock(spa);
			continue;
		}

		if (brtvd->bv_mos_brtvdev == 0)
			brt_vdev_create(spa, brtvd, tx);

		void *c = NULL;
		while ((bre = avl_destroy_nodes(&brtvd->bv_tree, &c)) != NULL) {
			brt_sync_entry(spa, brtvd->bv_mos_entries_dnode, bre,
			    tx);
			kmem_cache_free(brt_entry_cache, bre);
		}

#ifdef ZFS_DEBUG
		if (zfs_flags & ZFS_DEBUG_BRT)
			brt_vdev_dump(brtvd);
#endif
		if (brtvd->bv_totalcount == 0 && brtvd->bv_reloc_count == 0)
			brt_vdev_destroy(spa, brtvd, tx);
		else
			brt_vdev_sync(spa, brtvd, tx);
		brt_rlock(spa);
	}
	brt_unlock(spa);
}

void
brt_sync(spa_t *spa, uint64_t txg)
{
	dmu_tx_t *tx;
	uint64_t vdevid;

	ASSERT3U(spa_syncing_txg(spa), ==, txg);

	brt_rlock(spa);
	for (vdevid = 0; vdevid < spa->spa_brt_nvdevs; vdevid++) {
		if (spa->spa_brt_vdevs[vdevid]->bv_meta_dirty)
			break;
	}
	if (vdevid >= spa->spa_brt_nvdevs &&
	    list_is_empty(&spa->spa_brt_retire)) {
		brt_unlock(spa);
		return;
	}
	brt_unlock(spa);

	tx = dmu_tx_create_assigned(spa->spa_dsl_pool, txg);
	brt_sync_table(spa, tx);
	brt_reloc_sync(spa, tx);
	dmu_tx_commit(tx);
}

static void
brt_alloc(spa_t *spa)
{
	rw_init(&spa->spa_brt_lock, NULL, RW_DEFAULT, NULL);
	rw_init(&spa->spa_brt_reloc_lock, NULL, RW_DEFAULT, NULL);
	zfs_btree_create(&spa->spa_brt_reloc, brt_reloc_compare, NULL,
	    sizeof (brt_reloc_entry_t));
	zfs_btree_create(&spa->spa_brt_reloc_multi, brt_reloc_compare, NULL,
	    sizeof (brt_reloc_multi_t));
	rw_init(&spa->spa_brt_reloc_obj_lock, NULL, RW_DEFAULT, NULL);
	list_create(&spa->spa_brt_retire, sizeof (brt_reloc_retire_t),
	    offsetof(brt_reloc_retire_t, brr_node));
	spa->spa_brt_reloc_io = kmem_zalloc(BRT_RELOC_SHARDS *
	    sizeof (brt_reloc_shard_t), KM_SLEEP);
	for (int i = 0; i < BRT_RELOC_SHARDS; i++) {
		brt_reloc_shard_t *shard = &spa->spa_brt_reloc_io[i];
		mutex_init(&shard->brs_lock, NULL, MUTEX_DEFAULT, NULL);
		cv_init(&shard->brs_cv, NULL, CV_DEFAULT, NULL);
		list_create(&shard->brs_reads, sizeof (zio_t),
		    offsetof(zio_t, io_brt_node));
	}
	spa->spa_brt_vdevs = NULL;
	spa->spa_brt_nvdevs = 0;
	spa->spa_brt_rangesize = 0;
	spa->spa_brt_reloc_obj = 0;

	spa->spa_brt_dedup = kmem_zalloc(sizeof (brt_dedup_shard_t) *
	    BRT_DEDUP_SHARDS, KM_SLEEP);
	for (int i = 0; i < BRT_DEDUP_SHARDS; i++) {
		brt_dedup_shard_t *bds = &spa->spa_brt_dedup[i];
		mutex_init(&bds->bds_lock, NULL, MUTEX_DEFAULT, NULL);
		for (int t = 0; t < TXG_SIZE; t++) {
			avl_create(&bds->bds_tree[t], brt_entry_dedup_compare,
			    sizeof (brt_entry_t),
			    offsetof(brt_entry_t, bre_node));
		}
	}
}

void
brt_create(spa_t *spa)
{
	brt_alloc(spa);
	spa->spa_brt_rangesize = BRT_RANGESIZE;
}

int
brt_load(spa_t *spa)
{
	int error = 0;
	boolean_t reloc_active =
	    spa_feature_is_active(spa, SPA_FEATURE_BRT_RELOCATION);

	brt_alloc(spa);
	brt_wlock(spa);
	if (reloc_active) {
		error = zap_lookup(spa->spa_meta_objset,
		    DMU_POOL_DIRECTORY_OBJECT, BRT_RELOC_MAP_NAME,
		    sizeof (uint64_t), 1, &spa->spa_brt_reloc_obj);
		if (error == ENOENT) {
			spa->spa_brt_reloc_obj = 0;
			error = 0;
		}
	}
	for (uint64_t vdevid = 0; error == 0 &&
	    vdevid < spa->spa_root_vdev->vdev_children; vdevid++) {
		char name[64];
		uint64_t mos_brtvdev;

		/* Look if this vdev had active block cloning. */
		snprintf(name, sizeof (name), "%s%llu", BRT_OBJECT_VDEV_PREFIX,
		    (u_longlong_t)vdevid);
		error = zap_lookup(spa->spa_meta_objset,
		    DMU_POOL_DIRECTORY_OBJECT, name, sizeof (uint64_t), 1,
		    &mos_brtvdev);
		if (error == ENOENT) {
			error = 0;
			continue;
		}
		if (error != 0)
			break;

		/* If it did, then allocate them all and load this one. */
		brt_vdevs_expand(spa, spa->spa_root_vdev->vdev_children);
		brt_vdev_t *brtvd = spa->spa_brt_vdevs[vdevid];
		rw_enter(&brtvd->bv_lock, RW_WRITER);
		brtvd->bv_mos_brtvdev = mos_brtvdev;
		error = brt_vdev_load(spa, brtvd);
		rw_exit(&brtvd->bv_lock);
		if (error != 0)
			break;
	}
	if (error == 0 && reloc_active) {
		if (spa->spa_brt_reloc_obj != 0) {
			if (spa->spa_brt_nvdevs <
			    spa->spa_root_vdev->vdev_children) {
				brt_vdevs_expand(spa,
				    spa->spa_root_vdev->vdev_children);
			}
			error = brt_reloc_map_load(spa);
		} else {
			error = SET_ERROR(ECKSUM);
		}
	}

	uint64_t reloc_count = zfs_btree_numnodes(&spa->spa_brt_reloc) +
	    zfs_btree_numnodes(&spa->spa_brt_reloc_multi);
	if (error == 0 && reloc_active && reloc_count == 0)
		error = SET_ERROR(ECKSUM);
	if (spa->spa_brt_rangesize == 0)
		spa->spa_brt_rangesize = BRT_RANGESIZE;
	brt_unlock(spa);
	if (error == 0)
		atomic_add_64(&spa->spa_brt_reloc_count, reloc_count);
	return (error);
}

void
brt_prefetch_all(spa_t *spa)
{
	/*
	 * Load all BRT entries for each vdev. This is intended to perform
	 * a prefetch on all such blocks. For the same reason that brt_prefetch
	 * (called from brt_pending_add) isn't locked, this is also not locked.
	 */
	rw_enter(&spa->spa_brt_reloc_obj_lock, RW_READER);
	if (spa->spa_brt_reloc_obj != 0)
		(void) zap_prefetch_object(spa->spa_meta_objset,
		    spa->spa_brt_reloc_obj);
	rw_exit(&spa->spa_brt_reloc_obj_lock);
	brt_rlock(spa);
	for (uint64_t vdevid = 0; vdevid < spa->spa_brt_nvdevs; vdevid++) {
		brt_vdev_t *brtvd = spa->spa_brt_vdevs[vdevid];
		brt_unlock(spa);

		rw_enter(&brtvd->bv_mos_entries_lock, RW_READER);
		if (brtvd->bv_mos_entries != 0) {
			(void) zap_prefetch_object(spa->spa_meta_objset,
			    brtvd->bv_mos_entries);
		}
		rw_exit(&brtvd->bv_mos_entries_lock);

		brt_rlock(spa);
	}
	brt_unlock(spa);
}

void
brt_unload(spa_t *spa)
{
	if (spa->spa_brt_rangesize == 0)
		return;
	atomic_swap_64(&spa->spa_brt_reloc_count, 0);
	zfs_btree_clear(&spa->spa_brt_reloc);
	zfs_btree_destroy(&spa->spa_brt_reloc);
	zfs_btree_clear(&spa->spa_brt_reloc_multi);
	zfs_btree_destroy(&spa->spa_brt_reloc_multi);
	brt_reloc_retire_t *retire;
	while ((retire = list_remove_head(&spa->spa_brt_retire)) != NULL)
		kmem_free(retire, sizeof (*retire));
	list_destroy(&spa->spa_brt_retire);
	rw_destroy(&spa->spa_brt_reloc_obj_lock);
	rw_destroy(&spa->spa_brt_reloc_lock);
	for (int i = 0; i < BRT_RELOC_SHARDS; i++) {
		brt_reloc_shard_t *shard = &spa->spa_brt_reloc_io[i];
		ASSERT0P(shard->brs_draining);
		list_destroy(&shard->brs_reads);
		cv_destroy(&shard->brs_cv);
		mutex_destroy(&shard->brs_lock);
	}
	kmem_free(spa->spa_brt_reloc_io, BRT_RELOC_SHARDS *
	    sizeof (brt_reloc_shard_t));
	spa->spa_brt_reloc_io = NULL;
	spa->spa_brt_reloc_obj = 0;
	brt_vdevs_free(spa);
	rw_destroy(&spa->spa_brt_lock);
	spa->spa_brt_rangesize = 0;

	for (int i = 0; i < BRT_DEDUP_SHARDS; i++) {
		brt_dedup_shard_t *bds = &spa->spa_brt_dedup[i];
		for (int t = 0; t < TXG_SIZE; t++)
			avl_destroy(&bds->bds_tree[t]);
		mutex_destroy(&bds->bds_lock);
	}
	kmem_free(spa->spa_brt_dedup, sizeof (brt_dedup_shard_t) *
	    BRT_DEDUP_SHARDS);
	spa->spa_brt_dedup = NULL;
}

ZFS_MODULE_PARAM(zfs_brt, , brt_zap_prefetch, INT, ZMOD_RW,
	"Enable prefetching of BRT ZAP entries");
ZFS_MODULE_PARAM(zfs_brt, , brt_zap_default_bs, UINT, ZMOD_RW,
	"BRT ZAP leaf blockshift");
ZFS_MODULE_PARAM(zfs_brt, , brt_zap_default_ibs, UINT, ZMOD_RW,
	"BRT ZAP indirect blockshift");
