#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define WKY_IO_BUF_SIZE 65536

static char __wky_io_buf[WKY_IO_BUF_SIZE];
static size_t __wky_io_pos = 0;
static int __wky_io_inited = 0;
static int __wky_io_line_buffered = 0;

static const char __wky_digits_2[201] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

void __wky_flush(void) {
    if (__wky_io_pos > 0) {
        fwrite(__wky_io_buf, 1, __wky_io_pos, stdout);
        __wky_io_pos = 0;
    }
}

void __wky_io_init(void) {
    if (__wky_io_inited) return;
    __wky_io_inited = 1;
    __wky_io_line_buffered = isatty(1) ? 1 : 0;
    atexit(__wky_flush);
}

static inline void __wky_ensure_cap(size_t need) {
    if (__wky_io_pos + need > WKY_IO_BUF_SIZE) {
        __wky_flush();
    }
}

void __wky_print_str(const char *s, int64_t len) {
    if (!__wky_io_inited) __wky_io_init();
    if (len <= 0 || !s) return;
    if ((size_t)len > WKY_IO_BUF_SIZE) {
        __wky_flush();
        fwrite(s, 1, (size_t)len, stdout);
        return;
    }
    __wky_ensure_cap((size_t)len);
    memcpy(__wky_io_buf + __wky_io_pos, s, (size_t)len);
    __wky_io_pos += (size_t)len;
}

void __wky_print_cstr(const char *s) {
    if (!s) return;
    const char *p = s;
    while (*p) p++;
    __wky_print_str(s, (int64_t)(p - s));
}

void __wky_print_char(char c) {
    if (!__wky_io_inited) __wky_io_init();
    __wky_ensure_cap(1);
    __wky_io_buf[__wky_io_pos++] = c;
    if (c == '\n' && __wky_io_line_buffered) {
        __wky_flush();
    }
}

void __wky_print_nl(void) {
    __wky_print_char('\n');
}

void __wky_print_bool(int b) {
    if (b) {
        __wky_print_str("true", 4);
    } else {
        __wky_print_str("false", 5);
    }
}

void __wky_print_u64(uint64_t u) {
    if (!__wky_io_inited) __wky_io_init();
    __wky_ensure_cap(24);
    char tmp[24];
    char *p = tmp + sizeof(tmp);
    while (u >= 100) {
        uint64_t q = u / 100;
        uint32_t r = (uint32_t)(u - q * 100);
        u = q;
        p -= 2;
        memcpy(p, &__wky_digits_2[r * 2], 2);
    }
    if (u < 10) {
        *--p = '0' + (char)u;
    } else {
        p -= 2;
        memcpy(p, &__wky_digits_2[u * 2], 2);
    }
    size_t n = (size_t)((tmp + sizeof(tmp)) - p);
    memcpy(__wky_io_buf + __wky_io_pos, p, n);
    __wky_io_pos += n;
}

void __wky_print_i64(int64_t v) {
    if (v < 0) {
        __wky_print_char('-');
        __wky_print_u64(0 - (uint64_t)v);
    } else {
        __wky_print_u64((uint64_t)v);
    }
}

void __wky_print_hex(uint64_t u, int uppercase) {
    if (!__wky_io_inited) __wky_io_init();
    __wky_ensure_cap(20);
    static const char hex_lower[] = "0123456789abcdef";
    static const char hex_upper[] = "0123456789ABCDEF";
    const char *hex_digits = uppercase ? hex_upper : hex_lower;
    char tmp[20];
    char *p = tmp + sizeof(tmp);
    if (u == 0) {
        *--p = '0';
    } else {
        while (u > 0) {
            *--p = hex_digits[u & 0xf];
            u >>= 4;
        }
    }
    size_t n = (size_t)((tmp + sizeof(tmp)) - p);
    memcpy(__wky_io_buf + __wky_io_pos, p, n);
    __wky_io_pos += n;
}

void __wky_print_pad_i64(int64_t v, int width, char pad_char) {
    if (!__wky_io_inited) __wky_io_init();
    char tmp[32];
    int neg = 0;
    uint64_t u;
    if (v < 0) {
        neg = 1;
        u = 0 - (uint64_t)v;
    } else {
        u = (uint64_t)v;
    }
    char *p = tmp + sizeof(tmp);
    while (u >= 100) {
        uint64_t q = u / 100;
        uint32_t r = (uint32_t)(u - q * 100);
        u = q;
        p -= 2;
        memcpy(p, &__wky_digits_2[r * 2], 2);
    }
    if (u < 10) {
        *--p = '0' + (char)u;
    } else {
        p -= 2;
        memcpy(p, &__wky_digits_2[u * 2], 2);
    }
    int num_len = (int)((tmp + sizeof(tmp)) - p);
    int total_len = num_len + neg;
    int pad_len = width > total_len ? width - total_len : 0;
    if ((size_t)total_len + (size_t)pad_len > WKY_IO_BUF_SIZE) {
        if (neg && pad_char == '0') {
            __wky_print_char('-');
            neg = 0;
        }
        while (pad_len > 0) {
            size_t chunk = (size_t)pad_len > WKY_IO_BUF_SIZE ? WKY_IO_BUF_SIZE : (size_t)pad_len;
            __wky_ensure_cap(chunk);
            memset(__wky_io_buf + __wky_io_pos, pad_char, chunk);
            __wky_io_pos += chunk;
            pad_len -= (int)chunk;
        }
        if (neg) __wky_print_char('-');
        __wky_print_str(p, num_len);
        return;
    }
    __wky_ensure_cap((size_t)(total_len + pad_len));
    if (neg && pad_char == '0') {
        __wky_io_buf[__wky_io_pos++] = '-';
        neg = 0;
    }
    memset(__wky_io_buf + __wky_io_pos, pad_char, (size_t)pad_len);
    __wky_io_pos += (size_t)pad_len;
    if (neg) {
        __wky_io_buf[__wky_io_pos++] = '-';
    }
    memcpy(__wky_io_buf + __wky_io_pos, p, (size_t)num_len);
    __wky_io_pos += (size_t)num_len;
}

static void __wky_print_f64_format(double v, int prec, int fixed) {
    char tmp[64];
    int n = fixed ? snprintf(tmp, sizeof(tmp), "%.*f", prec, v) :
                    snprintf(tmp, sizeof(tmp), "%g", v);
    if (n <= 0) return;
    if ((size_t)n < sizeof(tmp)) {
        __wky_print_str(tmp, n);
        return;
    }
    size_t capacity = (size_t)n + 1;
    char *full = malloc(capacity);
    if (!full) abort();
    int written = fixed ? snprintf(full, capacity, "%.*f", prec, v) :
                          snprintf(full, capacity, "%g", v);
    if (written > 0 && (size_t)written < capacity) __wky_print_str(full, written);
    free(full);
}

void __wky_print_f64_prec(double v, int prec) {
    if (!__wky_io_inited) __wky_io_init();
    if (prec == 2 && v >= -1e14 && v <= 1e14 && v == v) {
        if (v == 0.0) {
            if (signbit(v)) __wky_print_char('-');
            __wky_print_str("0.00", 4);
            return;
        }
        if (v < 0) {
            __wky_print_char('-');
            v = -v;
        }
        uint64_t bits;
        memcpy(&bits, &v, sizeof(bits));
        int exp = (int)((bits >> 52) & 0x7ff) - 1023;
        uint64_t mant = (bits & 0xfffffffffffffull) | (1ull << 52);
        int shift = 52 - exp;
        unsigned __int128 num = (unsigned __int128)mant * 100;
        uint64_t rounded;
        if (shift > 0) {
            if (shift > 120) {
                __wky_print_str("0.00", 4);
                return;
            }
            unsigned __int128 half = ((unsigned __int128)1) << (shift - 1);
            unsigned __int128 rem = num & ((((unsigned __int128)1) << shift) - 1);
            rounded = (uint64_t)(num >> shift);
            if (rem > half) {
                rounded++;
            } else if (rem == half) {
                if (rounded & 1) rounded++;
            }
        } else {
            rounded = (uint64_t)(num << (-shift));
        }
        uint64_t int_p = rounded / 100;
        uint32_t frac_p = (uint32_t)(rounded % 100);
        __wky_print_u64(int_p);
        __wky_print_char('.');
        __wky_ensure_cap(2);
        memcpy(__wky_io_buf + __wky_io_pos, &__wky_digits_2[frac_p * 2], 2);
        __wky_io_pos += 2;
        return;
    }
    __wky_print_f64_format(v, prec, 1);
}

void __wky_print_f64(double v) {
    __wky_print_f64_format(v, 0, 0);
}
