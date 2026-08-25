#include <stdio.h>
#include <stdint.h>
#include <string.h>

// C twin of b5_strings.kawa: same corpus, same tokenize + FNV-1a over each
// word, content compare every 16th word. Words are (ptr,len) pairs -- the
// manual analogue of Kawa's fat str views.

static const char *CORPUS =
    "the quick brown fox jumps over the lazy dog the quick brown fox pack my "
    "box with five dozen liquor jugs pack my box with five how vexingly quick "
    "daft zebras jump how vexingly quick daft zebras sphinx of black quartz "
    "judge my vow sphinx of black quartz judge ";

static uint32_t fnv(const char *data, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h = (h ^ (uint32_t)(unsigned char)data[i]) * 16777619u;
    }
    return h;
}

int main(void) {
    const long long ROUNDS = 2000000;
    const uint32_t clen = (uint32_t)strlen(CORPUS);
    uint64_t acc = 0;
    uint32_t words = 0;
    const char *cd = CORPUS;
    for (long long r = 0; r < ROUNDS; r++) {
        uint32_t i = 0;
        while (i < clen) {
            while (i < clen && cd[i] == ' ') i++;
            uint32_t start = i;
            while (i < clen && cd[i] != ' ') i++;
            uint32_t wlen = i - start;
            if (wlen > 0) {
                acc += fnv(cd + start, wlen);
                words++;
                if ((words & 15u) == 0u && wlen == 5 &&
                    memcmp(cd + start, "quick", 5) == 0)
                    acc += 7;
            }
        }
        acc += (uint64_t)r;
    }
    printf("acc=%llu words=%u\n", (unsigned long long)acc, words);
    return 0;
}
