#include "hm.h"
#include <stdio.h>

int main() {
	fast_map_t map;
	fm_init(&map, 16);

	printf("1. Insert Keys 0-9\n");
	for (int i = 0; i < 10; i++)
		fm_put(&map, i, i * 100);
	// fm_debug_print(&map);

	printf("2. Delete Keys 0-4\n");
	for (int i = 0; i < 5; i++)
		fm_delete(&map, i);
	// fm_debug_print(&map);

	printf("3. Check Keys\n");
	fm_val_t v;

	// Check Deleted
	int fail = 0;
	for (int i = 0; i < 5; i++) {
		if (fm_get(&map, i, &v)) {
			printf("FAIL: Key %d still exists\n", i);
			fail = 1;
		}
	}

	// Check Remaining
	for (int i = 5; i < 10; i++) {
		if (!fm_get(&map, i, &v)) {
			printf("FAIL: Key %d missing\n", i);
			fail = 1;
		}
	}

	if (!fail)
		printf("INTEGRITY PASS: All keys consistent.\n");

	printf("4. Re-insert Key 2\n");
	fm_put(&map, 2, 999);
	if (fm_get(&map, 2, &v) && v == 999)
		printf("PASS: Reuse tombstone OK\n");
	else
		printf("FAIL: Reuse tombstone error\n");

	fm_free(&map);
	return 0;
}
