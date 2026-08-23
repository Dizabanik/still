#include <stdint.h>
#include <stdio.h>

static inline uint32_t hash_step(uint32_t h, uint32_t v) { return h * 31 + v; }

int main() {
	uint32_t data[256];
	for (uint32_t i = 0; i < 256; i++)
		data[i] = i * 2654435761u;

	uint64_t total = 0;
	for (uint64_t r = 0; r < 3000000; r++) {
		uint32_t h = 2166136261u;
		for (int i = 0; i < 256; i++)
			h = hash_step(h, data[i]);
		total += h;
	}
	printf("%llu\n", (unsigned long long)total);
}
