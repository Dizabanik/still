#include <assert.h>
#include <float.h>
#include <stdint.h>
#include <string.h>

void __wky_print_str(const char *, int64_t);
void __wky_print_i64(int64_t);
void __wky_print_pad_i64(int64_t, int, char);
void __wky_print_f64_prec(double, int);
void __wky_print_nl(void);

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "integers")) {
        __wky_print_i64(INT64_MIN);
        __wky_print_nl();
        __wky_print_pad_i64(INT64_MIN, 22, '0');
        __wky_print_nl();
        __wky_print_pad_i64(INT64_MIN, 22, ' ');
        __wky_print_nl();
    } else if (!strcmp(argv[1], "padding")) {
        char prefix[65530];
        memset(prefix, 'p', sizeof(prefix));
        __wky_print_str(prefix, sizeof(prefix));
        __wky_print_nl();
        int widths[] = {0, 1, 65535, 65536, 65537, 131073};
        int values[] = {7, -7};
        char fills[] = {' ', '0'};
        for (unsigned w = 0; w < sizeof(widths) / sizeof(*widths); ++w)
            for (unsigned v = 0; v < sizeof(values) / sizeof(*values); ++v)
                for (unsigned f = 0; f < sizeof(fills) / sizeof(*fills); ++f) {
                    __wky_print_pad_i64(values[v], widths[w], fills[f]);
                    __wky_print_nl();
                }
    } else if (!strcmp(argv[1], "floats")) {
        int precisions[] = {0, 2, 6, 63, 64, 100, 4096};
        double values[] = {1.25, -1.25, DBL_MAX, -0.0, 0.125, 0.375};
        for (unsigned p = 0; p < sizeof(precisions) / sizeof(*precisions); ++p)
            for (unsigned v = 0; v < sizeof(values) / sizeof(*values); ++v) {
                __wky_print_f64_prec(values[v], precisions[p]);
                __wky_print_nl();
            }
    } else {
        assert(!strcmp(argv[1], "bounded"));
        char bytes[131079];
        memset(bytes, 'x', sizeof(bytes));
        __wky_print_str(bytes, sizeof(bytes));
        __wky_print_nl();
        __wky_print_str("a\0b", 3);
        __wky_print_nl();
        __wky_print_str(NULL, 0);
        __wky_print_str(bytes + 3, 2);
        __wky_print_nl();
    }
    return 0;
}
