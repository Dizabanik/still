#ifndef FASTMAP_H
#define FASTMAP_H

/*
 * FASTMAP - High Performance Swiss Table Hash Map
 * Supports:
 * - x86_64 (AVX2/SSE2)
 * - ARM64 (NEON - Apple Silicon M1/M2/M3 & Linux)
 * - Generic (Portable C fallback)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- 1. ARCHITECTURE DETECTION --- */

#if defined(__x86_64__) || defined(_M_X64)
#define FM_ARCH_X86
#include <emmintrin.h>
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
#define FM_ARCH_ARM
#include <arm_neon.h>
#else
#define FM_ARCH_GENERIC
#endif

/* --- 2. COMPILER COMPATIBILITY --- */

#if defined(_MSC_VER)
#include <intrin.h>
#include <malloc.h>
#define FM_INLINE __forceinline
#define FM_ALIGNED_FREE(ptr) _aligned_free((ptr))
#define FM_LIKELY(x) (x)
#define FM_UNLIKELY(x) (x)

static inline int fm_ctz(uint32_t val) {
	unsigned long idx;
	if (_BitScanForward(&idx, val))
		return (int)idx;
	return 32;
}
#define FM_ALIGNED_ALLOC(ptr, size, align)                                     \
	*(ptr) = _aligned_malloc((size), (align))
#else
#define FM_INLINE __attribute__((always_inline)) inline
#define FM_ALIGNED_FREE(ptr) free((ptr))
#define FM_LIKELY(x) __builtin_expect(!!(x), 1)
#define FM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define fm_ctz(x) __builtin_ctz((x))

static inline void fm_aligned_alloc_wrap(void **ptr, size_t size,
										 size_t align) {
	if (posix_memalign(ptr, align, size) != 0)
		*ptr = NULL;
}
#define FM_ALIGNED_ALLOC(ptr, size, align)                                     \
	fm_aligned_alloc_wrap((void **)(ptr), (size), (align))
#endif

/* --- 3. CONSTANTS --- */
#define FM_LOAD_FACTOR 0.85
#define FM_CTRL_EMPTY 0xFF
#define FM_CTRL_DELETED 0xFE
#define FM_H2(hash) ((hash) >> 57)

typedef uint64_t fm_key_t;
typedef uint64_t fm_val_t;

typedef struct {
	fm_key_t key;
	fm_val_t val;
} fm_pair_t;

typedef struct {
	int8_t *ctrl;
	fm_pair_t *slots;
	size_t size;
	size_t capacity;
	size_t resize_threshold;
	size_t tombstones;
} fast_map_t;

/* --- 4. SIMD IMPLEMENTATIONS --- */

#if defined(FM_ARCH_X86)

#define FM_GROUP_WIDTH 16
typedef __m128i fm_vec_t;

FM_INLINE fm_vec_t fm_set1(int8_t c) { return _mm_set1_epi8(c); }
FM_INLINE fm_vec_t fm_load(const void *p) {
	return _mm_loadu_si128((const __m128i *)p);
}

FM_INLINE uint32_t fm_match(fm_vec_t ctrl, fm_vec_t target) {
	return (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, target));
}

FM_INLINE uint32_t fm_match_empty_or_del(fm_vec_t ctrl) {
	__m128i e = _mm_cmpeq_epi8(ctrl, _mm_set1_epi8((int8_t)FM_CTRL_EMPTY));
	__m128i d = _mm_cmpeq_epi8(ctrl, _mm_set1_epi8((int8_t)FM_CTRL_DELETED));
	return (uint32_t)_mm_movemask_epi8(_mm_or_si128(e, d));
}

#elif defined(FM_ARCH_ARM)

#define FM_GROUP_WIDTH 16
typedef uint8x16_t fm_vec_t;

FM_INLINE fm_vec_t fm_set1(int8_t c) { return vdupq_n_u8(c); }
FM_INLINE fm_vec_t fm_load(const void *p) {
	return vld1q_u8((const uint8_t *)p);
}

// Accurate NEON bitmask using horizontal sum of powers-of-2
FM_INLINE uint32_t fm_neon_movemask(uint8x16_t input) {
	const uint8_t masks[16] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
							   0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
	uint8x16_t bit_mask = vld1q_u8(masks);
	uint8x16_t masked = vandq_u8(input, bit_mask);
	uint8x8_t low = vget_low_u8(masked);
	uint8x8_t high = vget_high_u8(masked);
	return (uint32_t)(vaddv_u8(low) + (vaddv_u8(high) << 8));
}

FM_INLINE uint32_t fm_match(fm_vec_t ctrl, fm_vec_t target) {
	return fm_neon_movemask(vceqq_u8(ctrl, target));
}

FM_INLINE uint32_t fm_match_empty_or_del(fm_vec_t ctrl) {
	uint8x16_t e = vceqq_u8(ctrl, vdupq_n_u8((int8_t)FM_CTRL_EMPTY));
	uint8x16_t d = vceqq_u8(ctrl, vdupq_n_u8((int8_t)FM_CTRL_DELETED));
	return fm_neon_movemask(vorrq_u8(e, d));
}

#else
// GENERIC FALLBACK (SWAR) - CORRECTED
#define FM_GROUP_WIDTH 8
typedef uint64_t fm_vec_t;
#define FM_U64_H1 0x8080808080808080ULL
#define FM_U64_L1 0x0101010101010101ULL

FM_INLINE fm_vec_t fm_set1(int8_t c) {
	return (uint64_t)((uint8_t)c) * FM_U64_L1;
}
FM_INLINE fm_vec_t fm_load(const void *p) { return *(const uint64_t *)p; }

FM_INLINE uint32_t fm_match(fm_vec_t ctrl, fm_vec_t target) {
	uint64_t x = ctrl ^ target;
	// SWAR: Detect zero bytes
	uint64_t z = (x - FM_U64_L1) & ~x & FM_U64_H1;

	// Convert 0x80 markers into a compact bitmask
	uint32_t mask = 0;
	if (z) {
		// Unrolled extraction for safety and portability
		if (z & 0x0000000000000080ULL)
			mask |= 1;
		if (z & 0x0000000000008000ULL)
			mask |= 2;
		if (z & 0x0000000000800000ULL)
			mask |= 4;
		if (z & 0x0000000080000000ULL)
			mask |= 8;
		if (z & 0x0000008000000000ULL)
			mask |= 16;
		if (z & 0x0000800000000000ULL)
			mask |= 32;
		if (z & 0x0080000000000000ULL)
			mask |= 64;
		if (z & 0x8000000000000000ULL)
			mask |= 128;
	}
	return mask;
}

FM_INLINE uint32_t fm_match_empty_or_del(fm_vec_t ctrl) {
	return fm_match(ctrl, fm_set1((int8_t)FM_CTRL_EMPTY)) |
		   fm_match(ctrl, fm_set1((int8_t)FM_CTRL_DELETED));
}
#endif

/* --- 5. CORE LOGIC --- */

FM_INLINE uint64_t fm_wyhash(uint64_t A) {
	A ^= 0xa0761d6478bd642fULL;
#if defined(__SIZEOF_INT128__)
	__int128 r = A;
	r *= 0xa0761d6478bd642fULL;
	return (uint64_t)(r) ^ (uint64_t)(r >> 64);
#else
	A ^= A >> 33;
	A *= 0xff51afd7ed558ccdULL;
	A ^= A >> 33;
	A *= 0xc4ceb9fe1a85ec53ULL;
	return A ^ (A >> 33);
#endif
}

static void fm_init(fast_map_t *map, size_t cap) {
	if (cap < 16)
		cap = 16;
	size_t n = 1;
	while (n < cap)
		n <<= 1;
	cap = n;
	map->capacity = cap;
	map->size = 0;
	map->tombstones = 0;
	map->resize_threshold = (size_t)(cap * FM_LOAD_FACTOR);
	FM_ALIGNED_ALLOC(&map->ctrl, cap + FM_GROUP_WIDTH, 16);
	memset(map->ctrl, FM_CTRL_EMPTY, cap + FM_GROUP_WIDTH);
	FM_ALIGNED_ALLOC(&map->slots, cap * sizeof(fm_pair_t), 16);
}

static void fm_free(fast_map_t *map) {
	if (map->ctrl)
		FM_ALIGNED_FREE(map->ctrl);
	if (map->slots)
		FM_ALIGNED_FREE(map->slots);
	map->ctrl = NULL;
	map->slots = NULL;
}

FM_INLINE size_t fm_find_idx(fast_map_t *map, fm_key_t key) {
	uint64_t hash = fm_wyhash(key);
	int8_t h2 = (int8_t)(FM_H2(hash) & 0x7F);
	size_t mask = map->capacity - 1;
	size_t idx = hash & mask;
	fm_vec_t target = fm_set1(h2);
	fm_vec_t empty_marker = fm_set1((int8_t)FM_CTRL_EMPTY);

	while (1) {
		fm_vec_t ctrl = fm_load(map->ctrl + idx);
		uint32_t match = fm_match(ctrl, target);

		while (match) {
			int bit = fm_ctz(match);
			size_t probe = (idx + bit) & mask;
			if (FM_LIKELY(map->slots[probe].key == key))
				return probe;
			match &= ~(1 << bit);
		}

		if (fm_match(ctrl, empty_marker))
			return map->capacity;
		idx = (idx + FM_GROUP_WIDTH) & mask;
	}
}

static int fm_get(fast_map_t *map, fm_key_t key, fm_val_t *out_val) {
	size_t idx = fm_find_idx(map, key);
	if (idx != map->capacity) {
		*out_val = map->slots[idx].val;
		return 1;
	}
	return 0;
}

static int fm_delete(fast_map_t *map, fm_key_t key) {
	size_t idx = fm_find_idx(map, key);
	if (idx == map->capacity)
		return 0;
	map->ctrl[idx] = (int8_t)FM_CTRL_DELETED;
	map->size--;
	map->tombstones++;
	return 1;
}

static void fm_put(fast_map_t *map, fm_key_t key, fm_val_t val); // Fwd Decl

static void fm_resize(fast_map_t *map, size_t new_cap) {
	fast_map_t new_map;
	fm_init(&new_map, new_cap);
	for (size_t i = 0; i < map->capacity; ++i) {
		if ((map->ctrl[i] & 0x80) == 0)
			fm_put(&new_map, map->slots[i].key, map->slots[i].val);
	}
	fm_free(map);
	*map = new_map;
}

static void fm_put(fast_map_t *map, fm_key_t key, fm_val_t val) {
	if (FM_UNLIKELY((map->size + map->tombstones) >= map->resize_threshold)) {
		if (map->size < map->resize_threshold / 2)
			fm_resize(map, map->capacity);
		else
			fm_resize(map, map->capacity * 2);
	}

	uint64_t hash = fm_wyhash(key);
	int8_t h2 = (int8_t)(FM_H2(hash) & 0x7F);
	size_t mask = map->capacity - 1;
	size_t idx = hash & mask;
	size_t first_del = map->capacity;
	fm_vec_t target = fm_set1(h2);
	fm_vec_t del_marker = fm_set1((int8_t)FM_CTRL_DELETED);
	fm_vec_t empty_marker = fm_set1((int8_t)FM_CTRL_EMPTY);

	while (1) {
		fm_vec_t ctrl = fm_load(map->ctrl + idx);

		uint32_t match = fm_match(ctrl, target);
		while (match) {
			int bit = fm_ctz(match);
			size_t probe = (idx + bit) & mask;
			if (map->slots[probe].key == key) {
				map->slots[probe].val = val;
				return;
			}
			match &= ~(1 << bit);
		}

		uint32_t dels = fm_match(ctrl, del_marker);
		if (dels && first_del == map->capacity)
			first_del = (idx + fm_ctz(dels)) & mask;

		uint32_t empties = fm_match(ctrl, empty_marker);
		if (empties) {
			size_t insert_idx;
			if (first_del != map->capacity) {
				insert_idx = first_del;
				map->tombstones--;
			} else {
				insert_idx = (idx + fm_ctz(empties)) & mask;
			}
			map->ctrl[insert_idx] = h2;
			map->slots[insert_idx].key = key;
			map->slots[insert_idx].val = val;
			map->size++;
			return;
		}
		idx = (idx + FM_GROUP_WIDTH) & mask;
	}
}

static void fm_debug_print(fast_map_t *map) {
	printf("Map Dump (Cap: %zu, Size: %zu):\n", map->capacity, map->size);
	for (size_t i = 0; i < map->capacity; i++) {
		uint8_t c = (uint8_t)map->ctrl[i];
		if (c == FM_CTRL_EMPTY)
			printf(".");
		else if (c == FM_CTRL_DELETED)
			printf("x");
		else
			printf("%d",
				   (int)(map->slots[i].key % 10)); // Print last digit of key
		if ((i + 1) % 16 == 0)
			printf("\n");
	}
	printf("\n");
}

#endif // FASTMAP_H
