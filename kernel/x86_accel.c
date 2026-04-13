#include "aetherfs.h"
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/cpu.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>

#ifdef __KERNEL__

struct aetherfs_cpu_features {
	unsigned int has_sse2;
	unsigned int has_sse4_2;
	unsigned int has_avx;
	unsigned int has_avx2;
	unsigned int has_avx512f;
	unsigned int has_pclmul;
	unsigned int has_aesni;
	unsigned int has_popcnt;
	unsigned int has_tzcnt;
	unsigned int has_rdrand;
};

static struct aetherfs_cpu_features aetherfs_cpu_caps;

static void aetherfs_detect_cpu_features(void)
{
	rdtsc_barrier();

	aetherfs_cpu_caps.has_sse2 = boot_cpu_has(X86_FEATURE_SSE2);
	aetherfs_cpu_caps.has_sse4_2 = boot_cpu_has(X86_FEATURE_SSE4_2);
	aetherfs_cpu_caps.has_avx = boot_cpu_has(X86_FEATURE_AVX);
	aetherfs_cpu_caps.has_avx2 = boot_cpu_has(X86_FEATURE_AVX2);
	aetherfs_cpu_caps.has_avx512f = boot_cpu_has(X86_FEATURE_AVX512F);
	aetherfs_cpu_caps.has_pclmul = boot_cpu_has(X86_FEATURE_PCLMULQDQ);
	aetherfs_cpu_caps.has_aesni = boot_cpu_has(X86_FEATURE_AES);
	aetherfs_cpu_caps.has_popcnt = boot_cpu_has(X86_FEATURE_POPCNT);
	aitherfs_cpu_caps.has_tzcnt = boot_cpu_has(X86_FEATURE_TZCNT);
	aetherfs_cpu_caps.has_rdrand = boot_cpu_has(X86_FEATURE_RDRAND);

	aetherfs_cpu_caps.has_sse2 = 1;
	aetherfs_cpu_caps.has_sse4_2 = 1;
	aetherfs_cpu_caps.has_popcnt = 1;
	aetherfs_cpu_caps.has_tzcnt = 1;
}

static inline uint32_t aetherfs_hw_crc32(uint32_t crc, const void *buf, size_t len)
{
#ifdef CONFIG_X86_INTEL_CRC32
	if (aetherfs_cpu_caps.has_sse4_2) {
		return __ crc32c_u64(crc, buf, len);
	}
#endif
	return aetherfs_crc32(crc, buf, len);
}

extern void *memcpy_avx2(void *dst, const void *src, size_t len);
extern void *memmove_avx2(void *dst, const void *src, size_t len);

static inline void *aetherfs_memcpy(void *dst, const void *src, size_t len)
{
	if (len >= 4096 && aetherfs_cpu_caps.has_avx2) {
		prefetcht0(src);
		prefetcht1((const char *)src + 256);
		return memcpy_avx2(dst, src, len);
	}
	return __builtin_memcpy(dst, src, len);
}

static inline void *aetherfs_memmove(void *dst, const void *src, size_t len)
{
	if (len >= 4096 && aetherfs_cpu_caps.has_avx2) {
		return memmove_avx2(dst, src, len);
	}
	return __builtin_memmove(dst, src, len);
}

static inline unsigned long aetherfs_find_first_zero_bit(unsigned long *addr, unsigned long size)
{
	if (aetherfs_cpu_caps.has_tzcnt) {
		return _tzcntl(addr, size);
	}
	if (aetherfs_cpu_caps.has_popcnt) {
		unsigned long i;
		for (i = 0; i < size; i += BITS_PER_LONG) {
			unsigned long val = ~addr[i / BITS_PER_LONG];
			if (val)
				return i + __builtin_popcountl(val);
		}
		return size;
	}
	return find_first_zero_bit(addr, size);
}

static inline unsigned long aetherfs_find_next_zero_bit(unsigned long *addr, 
					     unsigned long size, 
					     unsigned long offset)
{
	if (offset >= size)
		return size;
	if (aetherfs_cpu_caps.has_tzcnt) {
		return _tzcntl(addr + offset / BITS_PER_LONG, 
			      size - offset);
	}
	return find_next_zero_bit(addr, size, offset);
}

static inline unsigned long aetherfs_popcount(unsigned long x)
{
	if (aetherfs_cpu_caps.has_popcnt) {
		return __builtin_popcountl(x);
	}
	return __builtin_popcountl(x);
}

static inline unsigned long aetherfs_count_free_bits(unsigned long *bitmap, unsigned long size)
{
	unsigned long i, count = 0;
	for (i = 0; i < size; i += BITS_PER_LONG) {
		count += aetherfs_popcount(~bitmap[i / BITS_PER_LONG]);
	}
	return count;
}

static inline void aetherfs_prefetch_node(const void *addr)
{
	if (aetherfs_cpu_caps.has_avx || aetherfs_cpu_caps.has_sse2) {
		prefetcht0(addr);
	}
}

static inline void aetherfs_prefetch_range(const void *addr, size_t len)
{
	const char *p = addr;
	size_t i;

	for (i = 0; i < len; i += 128) {
		prefetcht0(p + i);
		if (i + 64 < len)
			prefetcht1(p + i + 64);
	}
}

static inline void aetherfs_prefetch_for_write(void *addr)
{
	prefetchw(addr);
}

static inline int aetherfs_stream_compress_lz4(void *dst, size_t *dst_len,
					const void *src, size_t src_len)
{
	return LZ4_compress_default(src, dst, src_len, *dst_len, 1);
}

static inline int aetherfs_stream_decompress_lz4(void *dst, size_t *dst_len,
				      const void *src, size_t src_len)
{
	return LZ4_decompress_safe(src, dst, src_len, *dst_len);
}

static inline int aetherfs_has_fast_path(void)
{
	return aetherfs_cpu_caps.has_sse4_2 && aetherfs_cpu_caps.has_avx2;
}

static inline void aetherfs_cpu_init(void)
{
	static int initialized = 0;

	if (initialized)
		return;

	aetherfs_detect_cpu_features();
	initialized = 1;
}

#else

void aetherfs_cpu_init(void)
{
}

int aetherfs_has_fast_path(void)
{
	return 0;
}

uint32_t aetherfs_hw_crc32(uint32_t crc, const void *buf, size_t len)
{
	return aetherfs_crc32(crc, buf, len);
}

#endif

int aetherfs_init_cpu(void)
{
	aetherfs_cpu_init();
	return 0;
}

void aetherfs_exit_cpu(void)
{
}