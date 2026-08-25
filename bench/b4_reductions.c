#include <stdio.h>
#include <stdint.h>
static int32_t xi[262144];
static float xf[131072];
static int64_t sum_i32(const int32_t *d, int64_t n) {
    int64_t a = 0;
    for (int64_t i = 0; i < n; i++) a += d[i];
    return a;
}
static float dot_f32(const float *a, const float *b, int64_t n) {
    double acc = 0;
    for (int64_t i = 0; i < n; i++) acc += (double)a[i] * (double)b[i];
    return (float)acc;
}
static int32_t max_i32(const int32_t *d, int64_t n) {
    int32_t m = d[0];
    for (int64_t i = 1; i < n; i++) if (d[i] > m) m = d[i];
    return m;
}
int main(void) {
    for (int32_t i = 0; i < 262144; i++) xi[i] = i % 977;
    for (int32_t i = 0; i < 131072; i++) xf[i] = (float)i * 0.001f;
    int64_t total = 0;
    for (int32_t r = 0; r < 2000; r++) total += sum_i32(xi, 262144);
    float d = 0;
    for (int32_t r = 0; r < 4000; r++) d += dot_f32(xf, xf, 131072);
    int32_t mx = 0;
    for (int32_t r = 0; r < 1000; r++) mx += max_i32(xi, 262144);
    printf("t=%lld d=%.1f m=%d\n", (long long)total, (double)d, mx);
    return 0;
}
