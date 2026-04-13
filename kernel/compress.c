#include "aetherfs.h"

#ifdef __KERNEL__
#include <linux/zstd.h>
#include <linux/lz4.h>
#else
#include <zstd.h>
#include <lz4.h>
#endif

enum {
	AETHERFS_COMPRESS_NONE = 0,
	AETHERFS_COMPRESS_LZ4 = 1,
	AETHERFS_COMPRESS_ZSTD = 2,
	AETHERFS_COMPRESS_LZMA = 3,
};

struct aetherfs_compress_config {
	__le32 c_algorithm;
	__le32 c_level;
	__le32 c_min_blocks;
	__le32 c_reserved;
};

static inline int aetherfs_compress_lz4(void *dst, size_t *dst_len, 
			       const void *src, size_t src_len)
{
#ifdef __KERNEL__
	return LZ4_compress_default(src, dst, src_len, *dst_len, 1);
#else
	return LZ4_compress_default(src, dst, src_len, *dst_len);
#endif
}

static inline int aetherfs_decompress_lz4(void *dst, size_t *dst_len,
				 const void *src, size_t src_len)
{
#ifdef __KERNEL__
	return LZ4_decompress_safe(src, dst, src_len, *dst_len);
#else
	return LZ4_decompress_safe(src, dst, src_len, *dst_len);
#endif
}

static inline int aetherfs_compress_zstd(void *dst, size_t *dst_len,
					const void *src, size_t src_len, int level)
{
	size_t ret;
#ifdef __KERNEL__
	ret = ZSTD_compress(dst, *dst_len, src, src_len, level);
#else
	ret = ZSTD_compress(dst, *dst_len, src, src_len, level);
#endif
	if (ZSTD_isError(ret))
		return -EIO;
	*dst_len = ret;
	return 0;
}

static inline int aetherfs_decompress_zstd(void *dst, size_t *dst_len,
					 const void *src, size_t src_len)
{
	size_t ret;
#ifdef __KERNEL__
	ret = ZSTD_decompress(dst, *dst_len, src, src_len);
#else
	ret = ZSTD_decompress(dst, *dst_len, src, src_len);
#endif
	if (ZSTD_isError(ret))
		return -EIO;
	*dst_len = ret;
	return 0;
}

int aetherfs_compress(struct buffer_head *bh, int algorithm, int level)
{
	void *compressed;
	size_t comp_len, orig_len;
	struct buffer_head *comp_bh;
	int err;

	if (!bh)
		return -EINVAL;

	orig_len = bh->b_size;
	comp_bh = sb_getblk(bh->b_bdev->bd_super, 0);
	if (!comp_bh)
		return -EIO;

	compressed = comp_bh->b_data;
	comp_len = orig_len;

	switch (algorithm) {
	case AETHERFS_COMPRESS_LZ4:
		err = aetherfs_compress_lz4(compressed, &comp_len, 
					   bh->b_data, orig_len);
		break;
	case AETHERFS_COMPRESS_ZSTD:
		err = aetherfs_compress_zstd(compressed, &comp_len,
					   bh->b_data, orig_len, level);
		break;
	default:
		brelse(comp_bh);
		return -EINVAL;
	}

	if (err || comp_len >= orig_len) {
		brelse(comp_bh);
		return 0;
	}

	memcpy(bh->b_data, compressed, comp_len);
	bh->b_size = comp_len;
	brelse(comp_bh);

	return 0;
}

int aetherfs_decompress(struct buffer_head *bh, int algorithm)
{
	void *decompressed;
	size_t decomp_len;
	struct buffer_head *decomp_bh;
	int err;

	if (!bh)
		return -EINVAL;

	decomp_bh = sb_getblk(bh->b_bdev->bd_super, 0);
	if (!decomp_bh)
		return -EIO;

	decomp_len = decomp_bh->b_size;
	decompressed = decomp_bh->b_data;

	switch (algorithm) {
	case AETHERFS_COMPRESS_LZ4:
		err = aetherfs_decompress_lz4(decompressed, &decomp_len,
					   bh->b_data, bh->b_size);
		break;
	case AETHERFS_COMPRESS_ZSTD:
		err = aetherfs_decompress_zstd(decompressed, &decomp_len,
					   bh->b_data, bh->b_size);
		break;
	default:
		brelse(decomp_bh);
		return -EINVAL;
	}

	if (err) {
		brelse(decomp_bh);
		return err;
	}

	memcpy(bh->b_data, decompressed, decomp_len);
	bh->b_size = decomp_len;
	brelse(decomp_bh);

	return 0;
}

int aetherfs_should_compress(struct inode *inode, size_t size)
{
	struct aetherfs_inode_info *ei;
	uint32_t flags;

	if (!inode || size < 4096)
		return 0;

	ei = AETHERFS_INODE(inode);
	flags = le32_to_cpu(ei->i_flags);

	if (flags & AETHERFS_F_COMPRESSED)
		return 1;

	return 0;
}