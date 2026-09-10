#include <stdio.h>
#include <stdint.h>

typedef enum {
    Shape_Circle,
    Shape_Rect,
    Shape_Point,
} ShapeKind;

typedef struct {
    ShapeKind tag;
    union {
        struct { int64_t radius; } circle;
        struct { int64_t width; int64_t height; } rect;
    } payload;
} Shape;

static inline int64_t eval_shape(Shape s, int64_t factor) {
    switch (s.tag) {
    case Shape_Circle:
        return s.payload.circle.radius * s.payload.circle.radius * 3 + factor;
    case Shape_Rect:
        return s.payload.rect.width * s.payload.rect.height + factor * 2;
    case Shape_Point:
        return factor;
    default:
        return 0;
    }
}

int main(void) {
    const int64_t ROUNDS = 20000000;
    int64_t acc = 0;
    for (int64_t r = 0; r < ROUNDS; r++) {
        int64_t mode = r & 3;
        Shape s;
        s.tag = Shape_Point;
        if (mode == 0) {
            s.tag = Shape_Circle;
            s.payload.circle.radius = (r & 127) + 1;
        } else if (mode == 1) {
            s.tag = Shape_Rect;
            s.payload.rect.width = (r & 63) + 1;
            s.payload.rect.height = ((r >> 2) & 63) + 1;
        } else if (mode == 2) {
            s.tag = Shape_Point;
        } else {
            s.tag = Shape_Rect;
            s.payload.rect.width = ((r >> 1) & 31) + 2;
            s.payload.rect.height = (r & 31) + 3;
        }

        int64_t val = eval_shape(s, r & 15);
        acc = (acc * 33 + val) & 2147483647;
    }

    printf("acc=%lld\n", (long long)acc);
    return 0;
}
