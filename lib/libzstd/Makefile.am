include $(top_srcdir)/config/Rules.am

AM_CFLAGS += $(DEBUG_STACKFLAGS) $(FRAME_LARGER_THAN)

DEFAULT_INCLUDES += \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/lib/libspl/include

lib_LTLIBRARIES = libzstd.la

USER_C =

KERNEL_C =
	decompress/zstd_decompress.c \
	decompress/huf_decompress.c  \
	compress/zstd_compress.c     \
	compress/zstd_opt.h          \
	compress/zstdmt_compress.c   \
	compress/huf_compress.c      \
	compress/zstdmt_compress.h   \
	compress/fse_compress.c      \
	common/fse.h                 \
	common/huf.h                 \
	common/threading.h           \
	common/error_private.c       \
	common/fse_decompress.c      \
	common/zstd_common.c         \
	common/bitstream.h           \
	common/pool.c                \
	common/xxhash.h              \
	common/entropy_common.c      \
	common/threading.c           \
	common/xxhash.c              \
	common/zstd_internal.h       \
	common/zstd_errors.h         \
	common/pool.h                \
	common/mem.h                 \
	common/error_private.h

nodist_libzstd_la_SOURCES = \
	$(USER_C) \
	$(KERNEL_C)
