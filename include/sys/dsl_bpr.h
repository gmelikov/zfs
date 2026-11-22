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

#ifndef _SYS_DSL_BPR_H
#define	_SYS_DSL_BPR_H

#include <sys/types.h>

#define	BPR_BATCH_CURSOR_WORDS	6

typedef struct bpr_batch_cursor {
	uint64_t	bc_vdev_guid;
	uint64_t	bc_max_birth;
	uint64_t	bc_dsobj;
	uint64_t	bc_ds_guid;
	uint64_t	bc_object;
	uint64_t	bc_blkid;
} bpr_batch_cursor_t;

typedef struct bpr_batch_result {
	bpr_batch_cursor_t	br_cursor;
	uint64_t		br_bytes;
	uint64_t		br_blocks;
	uint64_t		br_skipped;
	uint64_t		br_visited;
	boolean_t		br_done;
	int			br_error;
} bpr_batch_result_t;

int dsl_bpr_batch(const char *, uint64_t, uint64_t, bpr_batch_result_t *);

#endif
