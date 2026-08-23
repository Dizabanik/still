#include <stdio.h>

#define N 1000000
#define DT 0.000001

typedef struct { double x, y, vx, vy; } P;

static P parts[N];

static inline double accel(double d) {
    return d * 9.8 - 0.01 * d * d;
}

int main(void) {
    for (int i = 0; i < N; i++) {
        parts[i].x = (double)i * 0.001;
        parts[i].y = (double)(i & 1023) * 0.5;
        parts[i].vx = 1.0;
        parts[i].vy = 0.5;
    }
    for (int s = 0; s < 100; s++) {
        for (int i = 0; i < N; i++) {
            parts[i].vx += accel(parts[i].x) * DT;
            parts[i].vy += accel(parts[i].y) * DT;
            parts[i].x += parts[i].vx * DT;
            parts[i].y += parts[i].vy * DT;
        }
    }
    double acc = 0;
    for (int i = 0; i < N; i++) {
        acc += parts[i].x;
    }
    printf("acc=%.6f\n", acc);
    return 0;
}
