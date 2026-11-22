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

#include <sys/abd.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/brt.h>
#include <sys/ddt.h>
#include <sys/dmu_tx.h>
#include <sys/dnode.h>
#include <sys/dsl_bpr.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_synctask.h>
#include <sys/metaslab_impl.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_rebuild.h>
#include <sys/zfeature.h>
#include <sys/zio_checksum.h>
#include <sys/zio_crypt.h>

static int
bpr_check_pool(dsl_pool_t *dp)
{
	spa_t *spa = dp->dp_spa;
	if (!spa_feature_is_enabled(spa, SPA_FEATURE_BRT_RELOCATION))
		return (SET_ERROR(ENOTSUP));
	if (spa->spa_checkpoint_txg != 0 || spa->spa_vdev_removal != NULL ||
	    dsl_scan_scrubbing(dp) || dsl_scan_resilvering(dp) ||
	    dsl_errorscrubbing(dp) ||
	    vdev_rebuild_active(spa->spa_root_vdev))
		return (SET_ERROR(EBUSY));
	return (0);
}

static int
bpr_copy_block(spa_t *spa, dmu_tx_t *tx, const blkptr_t *bp,
    abd_t *data, blkptr_t *new)
{
	zio_prop_t props = {
		.zp_checksum = BP_GET_CHECKSUM(bp),
		.zp_compress = BP_GET_COMPRESS(bp),
		.zp_complevel = ZIO_COMPLEVEL_DEFAULT,
		.zp_copies = BP_GET_NDVAS(bp),
		.zp_gang_copies = 1,
		.zp_type = BP_GET_TYPE(bp),
		.zp_level = BP_GET_LEVEL(bp),
	};
	uint64_t txg = dmu_tx_get_txg(tx);
	zio_flag_t flags = ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW_COMPRESS;
	if (BP_USES_CRYPT(bp)) {
		props.zp_encrypt = B_TRUE;
		props.zp_byteorder = BP_GET_BYTEORDER(bp);
		if (BP_IS_ENCRYPTED(bp))
			zio_crypt_decode_params_bp(bp, props.zp_salt,
			    props.zp_iv);
		zio_crypt_decode_mac_bp(bp, props.zp_mac);
		flags |= ZIO_FLAG_RAW_ENCRYPT;
	}
	BP_ZERO(new);
	zio_t *zio = zio_write(NULL, spa, txg, new, data,
	    BP_GET_LSIZE(bp), BP_GET_PSIZE(bp), &props,
	    NULL, NULL, NULL, NULL, ZIO_PRIORITY_SYNC_WRITE,
	    flags, NULL);
	zio->io_metaslab_class = spa_normal_class(spa);
	int error = zio_wait(zio);
	if (error != 0)
		return (error);

	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	boolean_t normal = B_TRUE;
	for (int i = 0; i < BP_GET_NDVAS(new); i++) {
		vdev_t *vd = vdev_lookup_top(spa,
		    DVA_GET_VDEV(&new->blk_dva[i]));
		if (vd == NULL || vd->vdev_mg == NULL ||
		    vd->vdev_mg->mg_class != spa_normal_class(spa)) {
			normal = B_FALSE;
			break;
		}
	}
	spa_config_exit(spa, SCL_VDEV, FTAG);

	if (BP_IS_HOLE(new) || BP_IS_EMBEDDED(new) || BP_COUNT_GANG(new) != 0 ||
	    BP_GET_NDVAS(new) != BP_GET_NDVAS(bp) || !normal ||
	    BP_GET_PSIZE(new) != BP_GET_PSIZE(bp) ||
	    BP_GET_ASIZE(new) != BP_GET_ASIZE(bp) ||
	    bp_get_dsize_sync(spa, new) != bp_get_dsize_sync(spa, bp) ||
	    !ZIO_CHECKSUM_EQUAL(new->blk_cksum, bp->blk_cksum)) {
		dsl_free(dmu_tx_pool(tx), txg, new);
		return (SET_ERROR(ENOTSUP));
	}
	return (0);
}

static int
bpr_relocate_block(spa_t *spa, dmu_tx_t *tx, const blkptr_t *bp)
{
	uint64_t size = BP_GET_PSIZE(bp);
	abd_t *data = abd_alloc_linear(size, B_TRUE);
	blkptr_t new;
	int error = zio_wait(zio_read(NULL, spa, bp, data, size, NULL, NULL,
	    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW,
	    NULL));
	if (error == 0)
		error = bpr_copy_block(spa, tx, bp, data, &new);
	abd_free(data);
	if (error != 0)
		return (error);

	error = brt_relocate(spa, bp, &new, tx);
	if (error != 0)
		dsl_free(dmu_tx_pool(tx), dmu_tx_get_txg(tx), &new);
	return (error);
}

static boolean_t
bpr_ddt_has_more_dvas(spa_t *spa, const blkptr_t *bp)
{
	if (!BP_GET_DEDUP(bp))
		return (B_FALSE);

	ddt_t *ddt = ddt_select(spa, bp);
	if (!(ddt->ddt_flags & DDT_FLAG_FLAT))
		return (B_FALSE);

	boolean_t more = B_FALSE;
	ddt_enter(ddt);
	ddt_entry_t *dde = ddt_lookup(ddt, bp, B_TRUE);
	if (dde != NULL) {
		ddt_phys_variant_t v = ddt_phys_select(ddt, dde, bp);
		if (v != DDT_PHYS_NONE) {
			int count = ddt_phys_dva_count(dde->dde_phys, v,
			    BP_IS_ENCRYPTED(bp));
			more = count > BP_GET_NDVAS(bp);
		}
	}
	ddt_exit(ddt);
	return (more);
}

#define	BPR_BATCH_MAX_BYTES	(64ULL << 20)
#define	BPR_BATCH_MAX_BLOCKS	1024
#define	BPR_BATCH_MAX_VISITS	16384

typedef struct bpr_batch {
	uint64_t		bb_guid;
	uint64_t		bb_vdev;
	uint64_t		bb_limit;
	dmu_tx_t		*bb_tx;
	bpr_batch_result_t	*bb_result;
	boolean_t		bb_paused;
} bpr_batch_t;

static int
bpr_batch_pause(bpr_batch_t *batch)
{
	/*
	 * On FreeBSD, ERESTART is TRAVERSE_VISIT_NO_CHILDREN. Return
	 * EINTR with an explicit pause flag so traversal saves its bookmark.
	 */
	batch->bb_paused = B_TRUE;
	return (EINTR);
}

static int
bpr_batch_visit(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) dnp;
	bpr_batch_t *batch = arg;
	bpr_batch_result_t *res = batch->bb_result;

	if (zilog != NULL || zb->zb_level == ZB_DNODE_LEVEL)
		return (0);
	if (res->br_visited == BPR_BATCH_MAX_VISITS ||
	    res->br_blocks == BPR_BATCH_MAX_BLOCKS)
		return (bpr_batch_pause(batch));
	res->br_visited++;

	if (!brt_reloc_eligible(bp) ||
	    DVA_GET_VDEV(&bp->blk_dva[0]) != batch->bb_vdev ||
	    BP_GET_PHYSICAL_BIRTH(bp) > res->br_cursor.bc_max_birth ||
	    brt_reloc_resolve(spa, bp, NULL))
		return (0);
	if (bpr_ddt_has_more_dvas(spa, bp)) {
		res->br_skipped++;
		return (0);
	}

	uint64_t asize = BP_GET_ASIZE(bp);
	if (asize > batch->bb_limit - res->br_bytes)
		return (res->br_blocks == 0 ? SET_ERROR(E2BIG) :
		    bpr_batch_pause(batch));

	/*
	 * The syncing traversal supplies the current pointer. Publishing
	 * its mapping here also makes later aliases in this batch skip it.
	 */
	int error = bpr_relocate_block(spa, batch->bb_tx, bp);
	if (error == 0) {
		res->br_bytes += asize;
		res->br_blocks++;
	} else if (error == ENOTSUP) {
		/* Continue if the allocation size cannot be preserved. */
		res->br_skipped++;
		return (0);
	}
	return (error);
}

static int
bpr_batch_dataset(dsl_pool_t *dp, bpr_batch_t *batch)
{
	bpr_batch_cursor_t *cursor = &batch->bb_result->br_cursor;
	dmu_object_info_t info;
	int error = dmu_object_info(dp->dp_meta_objset, cursor->bc_dsobj,
	    &info);
	if (error == ENOENT)
		return (0);
	if (error != 0)
		return (error);
	if (info.doi_bonus_type != DMU_OT_DSL_DATASET)
		return (0);

	dsl_dataset_t *ds;
	error = dsl_dataset_hold_obj(dp, cursor->bc_dsobj, FTAG, &ds);
	if (error != 0)
		return (error);

	if (!DS_IS_INCONSISTENT(ds)) {
		uint64_t guid = dsl_dataset_phys(ds)->ds_guid;
		zbookmark_phys_t bookmark = { 0 };
		if (cursor->bc_ds_guid == guid) {
			SET_BOOKMARK(&bookmark, cursor->bc_dsobj,
			    cursor->bc_object, 0, cursor->bc_blkid);
		}
		cursor->bc_ds_guid = guid;
		error = traverse_dataset_resume(ds, 0, &bookmark,
		    TRAVERSE_PRE | TRAVERSE_NO_DECRYPT, bpr_batch_visit, batch);
		cursor->bc_object = bookmark.zb_object;
		cursor->bc_blkid = bookmark.zb_blkid;
	}

	dsl_dataset_rele(ds, FTAG);
	return (error);
}

static void
bpr_batch_sync(void *arg, dmu_tx_t *tx)
{
	bpr_batch_t *batch = arg;
	bpr_batch_result_t *res = batch->bb_result;
	bpr_batch_cursor_t *cursor = &res->br_cursor;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	objset_t *mos = dp->dp_meta_objset;
	spa_t *spa = dp->dp_spa;
	int error = bpr_check_pool(dp);
	if (error != 0)
		goto out;

	uint64_t guid = 0;
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *vd = spa_lookup_by_guid(spa, batch->bb_guid, B_FALSE);
	if (vd != NULL && vd == vd->vdev_top && vd->vdev_mg != NULL &&
	    vd->vdev_mg->mg_class == spa_special_class(spa)) {
		guid = vd->vdev_guid;
		batch->bb_vdev = vd->vdev_id;
	}
	spa_config_exit(spa, SCL_VDEV, FTAG);

	if (guid == 0) {
		error = SET_ERROR(ENODEV);
		goto out;
	}
	uint64_t max_birth = dmu_tx_get_txg(tx) - 1;
	if (cursor->bc_vdev_guid == 0) {
		cursor->bc_vdev_guid = guid;
		cursor->bc_max_birth = max_birth;
	} else if (cursor->bc_vdev_guid != guid ||
	    cursor->bc_max_birth > max_birth) {
		error = SET_ERROR(ESTALE);
		goto out;
	}

	batch->bb_tx = tx;
	uint64_t object = cursor->bc_dsobj;
	if (object == 0)
		error = dmu_object_next(mos, &object, B_FALSE, 0);

	while (error == 0) {
		cursor->bc_dsobj = object;
		if (res->br_visited == BPR_BATCH_MAX_VISITS ||
		    res->br_blocks == BPR_BATCH_MAX_BLOCKS)
			return;

		res->br_visited++;
		error = bpr_batch_dataset(dp, batch);
		if (error == EINTR && batch->bb_paused)
			return;
		if (error != 0)
			goto out;

		cursor->bc_ds_guid = 0;
		cursor->bc_object = 0;
		cursor->bc_blkid = 0;
		error = dmu_object_next(mos, &object, B_FALSE, 0);
	}

	if (error == ESRCH) {
		res->br_done = B_TRUE;
		error = 0;
	}
out:
	res->br_error = error;
}

int
dsl_bpr_batch(const char *pool, uint64_t guid, uint64_t limit,
    bpr_batch_result_t *res)
{
	const bpr_batch_cursor_t *cursor = &res->br_cursor;
	if (limit < SPA_MINBLOCKSIZE || limit > BPR_BATCH_MAX_BYTES ||
	    cursor->bc_dsobj >= DN_MAX_OBJECT ||
	    (cursor->bc_vdev_guid == 0 &&
	    (cursor->bc_max_birth != 0 || cursor->bc_dsobj != 0)) ||
	    (cursor->bc_ds_guid == 0 &&
	    (cursor->bc_object != 0 || cursor->bc_blkid != 0)))
		return (SET_ERROR(EINVAL));

	bpr_batch_t batch = {
		.bb_guid = guid,
		.bb_limit = limit,
		.bb_result = res,
	};
	return (dsl_sync_task(pool, NULL, bpr_batch_sync, &batch,
	    8 + (P2ROUNDUP(limit, 1 << 14) >> 14), ZFS_SPACE_CHECK_NORMAL));
}
