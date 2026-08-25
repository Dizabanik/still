#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdint.h>
#include <ucontext.h>
#include <string.h>

// C twin of b6_chan.kawa: same four-stage pipeline over capacity-64 rings,
// but stages are real coroutines (ucontext) with blocking send/recv that
// swap back to the driver on contention -- the closest plain-C analogue of
// kawac's brew/sip channels.

#define N 2000000
#define CAP 64
#define STACK (64 * 1024)

typedef struct {
    int64_t buf[CAP];
    int head, count;
} chan_t;

static ucontext_t main_ctx;
static ucontext_t ctx[4];
static char stacks[4][STACK];
static int done[4];

static inline void yield_to_main(int self) {
    swapcontext(&ctx[self], &main_ctx);
}

static void chan_send(chan_t *c, int64_t v, int self) {
    while (c->count == CAP) // full: block until the consumer drains
        yield_to_main(self);
    c->buf[(c->head + c->count) % CAP] = v;
    c->count++;
}

static int64_t chan_recv(chan_t *c, int self) {
    while (c->count == 0) // empty: block until a sender fills
        yield_to_main(self);
    int64_t v = c->buf[c->head];
    c->head = (c->head + 1) % CAP;
    c->count--;
    return v;
}

static inline int64_t step(int64_t v) {
    return (int64_t)((uint64_t)(v * 3 + 1) ^ 0x8000000000000000ULL);
}

typedef struct {
    chan_t *in, *out;
    int id;
} stage_arg;

static uint64_t sum;
static chan_t c2s, c3s, c4s;
static stage_arg s2arg, s3arg;

static void producer(void) {
    for (int64_t i = 0; i < N; i++)
        chan_send(&c2s, step(i), 0);
}
static void stage2(void) {
    for (int64_t i = 0; i < N; i++) {
        int64_t v = chan_recv(&c2s, 1);
        chan_send(&c3s, step(v), 1);
    }
}
static void stage3(void) {
    for (int64_t i = 0; i < N; i++) {
        int64_t v = chan_recv(&c3s, 2);
        chan_send(&c4s, step(v), 2);
    }
}
static void sink(void) {
    for (int64_t i = 0; i < N; i++)
        sum += (uint64_t)chan_recv(&c4s, 3);
    done[3] = 1;
}

int main(void) {
    static const void (*bodies[4])(void) = {producer, stage2, stage3, sink};
    for (int i = 0; i < 4; i++) {
        getcontext(&ctx[i]);
        ctx[i].uc_stack.ss_sp = stacks[i];
        ctx[i].uc_stack.ss_size = STACK;
        ctx[i].uc_link = &main_ctx;
        makecontext(&ctx[i], (void (*)(void))bodies[i], 0);
    }
    while (!done[3]) {
        for (int i = 0; i < 4; i++)
            if (!done[i])
                swapcontext(&main_ctx, &ctx[i]);
    }
    printf("sum=%llu\n", (unsigned long long)sum);
    return 0;
}
