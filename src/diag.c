#include "diag.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One-shot pending sub-diagnostics, consumed by the next error/warn.
static char *pending_help;
static char *pending_note;

// Program source text (set once by the driver) for kdiag_line().
static const char *source_text;
static int source_len;

void kdiag_set_source(const char *text, int len) {
	source_text = text;
	source_len = len;
}

char *kdiag_line(int line_num) {
	char *out = malloc(1);
	out[0] = '\0';
	if (!source_text || line_num <= 0)
		return out;
	// Walk to the start of the requested line, then copy through its end.
	const char *p = source_text, *end = source_text + source_len;
	int ln = 1;
	while (p < end && ln < line_num) {
		if (*p == '\n')
			ln++;
		p++;
	}
	if (p >= end)
		return out;
	const char *e = p;
	while (e < end && *e != '\n')
		e++;
	while (e > p && (e[-1] == '\r'))
		e--;
	free(out);
	out = malloc((size_t)(e - p) + 1);
	memcpy(out, p, (size_t)(e - p));
	out[e - p] = '\0';
	return out;
}

int kdiag_error_count(void) { return timbr_error_count; }
int kdiag_warn_count(void) { return timbr_warning_count; }

static void vappend(char **buf, const char *fmt, va_list ap) {
	char tmp[512];
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	size_t old = *buf ? strlen(*buf) : 0;
	*buf = realloc(*buf, old + strlen(tmp) + 1);
	if (!*buf)
		return;
	memcpy(*buf + old, tmp, strlen(tmp) + 1);
}

void kdiag_help(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vappend(&pending_help, fmt, ap);
	va_end(ap);
}

void kdiag_note(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vappend(&pending_note, fmt, ap);
	va_end(ap);
}

static void emit(int level, int code, const char *filename,
				 const char *code_line, int line_num, int col_num, int len,
				 const char *fmt, va_list ap) {
	char title[512];
	vsnprintf(title, sizeof(title), fmt, ap);

	char code_buf[8];
	const char *code_str = NULL;
	if (code > 0) {
		snprintf(code_buf, sizeof(code_buf), "E%04d", code);
		code_str = code_buf;
	}

	TimbrSpan span = {.code_line = code_line,
					  .filename = filename ? filename : "<kawa>",
					  .line_num = line_num,
					  .col_num = col_num,
					  .len = len > 0 ? len : 1,
					  .label = NULL,
					  .is_secondary = 0};

	const char *notes[2] = {pending_note, NULL};
	const char *helps[2] = {pending_help, NULL};
	timbr_diagnostic_ex(level, code_str, title, &span, 1, notes, helps);

	free(pending_help);
	free(pending_note);
	pending_help = NULL;
	pending_note = NULL;
}

void kdiag_error(int code, const char *filename, const char *code_line,
				 int line_num, int col_num, int len, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_ERROR, code, filename, code_line, line_num, col_num, len, fmt,
		 ap);
	va_end(ap);
}

void kdiag_error_at(int code, const char *filename, const char *code_line,
					int line_num, const char *fmt, ...) {
	char *owned = NULL;
	if (!code_line)
		code_line = owned = kdiag_line(line_num);
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_ERROR, code, filename, code_line, line_num, 1, 1, fmt, ap);
	va_end(ap);
	free(owned);
}

void kdiag_warn_at(int code, const char *filename, const char *code_line,
				   int line_num, const char *fmt, ...) {
	char *owned = NULL;
	if (!code_line)
		code_line = owned = kdiag_line(line_num);
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_WARN, code, filename, code_line, line_num, 1, 1, fmt, ap);
	va_end(ap);
	free(owned);
}

// First whole-word occurrence of `name` on the line: returns 1-based col
// and its length. Whole-word = bounded by non-identifier chars so `count`
// never matches inside `recount`.
static int find_name_col(const char *line, const char *name, int *out_len) {
	*out_len = 1;
	if (!line || !name || !*name)
		return 1;
	size_t nl = strlen(name);
	int vis = 1; // visual column, tabs expanded to 4 like the renderer
	for (const char *p = line; *p; p++) {
		if (strncmp(p, name, nl) == 0) {
			int before = (p == line) ? 0 : p[-1];
			int after = p[nl];
			int ident_before = isalnum((unsigned char)before) ||
							   before == '_';
			int ident_after =
				isalnum((unsigned char)after) || after == '_';
			if (!ident_before && !ident_after) {
				*out_len = (int)nl;
				return vis;
			}
		}
		vis += (*p == '\t') ? (4 - ((vis - 1) % 4)) : 1;
	}
	return 1;
}

static void emit_named(int level, int code, const char *filename,
					   int line_num, const char *name, const char *fmt,
					   va_list ap) {
	char *code_line = kdiag_line(line_num);
	int len;
	int col = find_name_col(code_line, name, &len);
	emit(level, code, filename, code_line, line_num, col, len, fmt, ap);
	free(code_line);
}

void kdiag_error_named(int code, const char *filename, int line_num,
					   const char *name, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	emit_named(TIMBR_ERROR, code, filename, line_num, name, fmt, ap);
	va_end(ap);
}

void kdiag_warn(int code, const char *filename, const char *code_line,
				int line_num, int col_num, int len, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_WARN, code, filename, code_line, line_num, col_num, len, fmt,
		 ap);
	va_end(ap);
}

void kdiag_summary(void) {
	FILE *f = timbr_config.output_stream ? timbr_config.output_stream : stderr;
	bool color =
		timbr_config.use_color;
	const char *bold = color ? "\033[1m" : "";
	const char *red = color ? "\033[31m" : "";
	const char *yellow = color ? "\033[33m" : "";
	const char *reset = color ? "\033[0m" : "";
	int e = timbr_error_count, w = timbr_warning_count;
	if (e > 0) {
		fprintf(f, "%s%serror%s: could not compile due to %d previous "
				   "error%s%s\n",
				bold, red, reset, e, e == 1 ? "" : "s",
				w > 0 ? "" : "");
	} else if (w > 0) {
		fprintf(f, "%s%swarning%s: %d warning%s emitted\n", bold, yellow,
				reset, w, w == 1 ? "" : "s");
	}
}

// Damerau-ish Levenshtein without transpositions: good enough for
// did-you-mean over identifiers. Caps at 3 for early exit.
static int edit_distance(const char *a, const char *b) {
	int la = (int)strlen(a), lb = (int)strlen(b);
	static int row[256];
	if (lb >= 256 || la >= 256)
		return 9999;
	for (int j = 0; j <= lb; j++)
		row[j] = j;
	for (int i = 1; i <= la; i++) {
		int prev_diag = row[0], prev_left;
		row[0] = i;
		for (int j = 1; j <= lb; j++) {
			prev_left = row[j];
			int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			int m = row[j - 1] + 1;	  // insert
			if (row[j] + 1 < m)
				m = row[j] + 1;		  // delete
			if (prev_diag + cost < m)
				m = prev_diag + cost; // substitute
			row[j] = m;
			prev_diag = prev_left;
		}
	}
	return row[lb];
}

const char *kdiag_closest(const char *needle, const char *const *candidates) {
	if (!needle)
		return NULL;
	int best = 9999;
	const char *best_c = NULL;
	for (const char *const *c = candidates; c && *c; c++) {
		int d = edit_distance(needle, *c);
		if (d < best) {
			best = d;
			best_c = *c;
		}
	}
	if (best <= 2)
		return best_c;
	return NULL;
}
