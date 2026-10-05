#include "diag.h"
#include "ast.h"
#include "driver.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One-shot pending sub-diagnostics, consumed by the next error/warn.
static char *pending_help;
static char *pending_note;

// Program source text (set once by the driver) for still_diag_line().
static const char *source_text;
static int source_len;
static int json_diagnostics;
typedef struct { const char *filename; int original_line, start, end; int owns_filename; } SourceLocation;
static SourceLocation *source_locations;
static int source_location_count;

void still_diag_set_json(int enabled) { json_diagnostics=enabled; }

void still_diag_json_string(FILE *out,const char *text) {
	fputc('"',out);
	for (const unsigned char *p=(const unsigned char *)(text ? text : ""); *p; ++p) {
		if (*p=='"' || *p=='\\') { fputc('\\',out); fputc(*p,out); }
		else if (*p<32) fprintf(out,"\\u%04x",*p);
		else if (*p<128) fputc(*p,out);
		else {
			unsigned bytes=(*p>=0xc2 && *p<=0xdf) ? 2 : (*p>=0xe0 && *p<=0xef) ? 3 : (*p>=0xf0 && *p<=0xf4) ? 4 : 0;
			uint32_t scalar=bytes ? (*p & (0x7f>>bytes)) : 0;
			for (unsigned i=1; i<bytes; ++i) {
				if ((p[i] & 0xc0)!=0x80) { bytes=0; break; }
				scalar=(scalar<<6) | (p[i] & 0x3f);
			}
			if ((bytes==2 && scalar<0x80) || (bytes==3 && scalar<0x800) ||
				(bytes==4 && scalar<0x10000) || (scalar>=0xd800 && scalar<=0xdfff) || scalar>0x10ffff)
				bytes=0;
			if (bytes) { fwrite(p,1,bytes,out); p+=bytes-1; }
			else fprintf(out,"\\u%04x",*p); // valid JSON even for arbitrary filename/source bytes
		}
	}
	fputc('"',out);
}
#define json_string still_diag_json_string

void still_diag_set_source(const char *text, int len) {
	source_text = text;
	source_len = len;
	for (int i=1; i<=source_location_count; ++i)
		if (source_locations && source_locations[i].owns_filename) free((void *)source_locations[i].filename);
	free(source_locations);
	source_location_count=1;
	for (int i=0; i<len; ++i) source_location_count += text[i]=='\n';
	source_locations=calloc((size_t)source_location_count+1,sizeof(*source_locations));
	const char *filename=NULL;
	int original=1,line=1;
	for (int i=0; i<len;) {
		int start=i;
		while (i<len && text[i]!='\n') ++i;
		char *path;
		if (driver_module_location(text+start,(size_t)(i-start),&path,&original)) {
			filename=path;
			source_locations[line++]=(SourceLocation){filename,original,start,i,1};
			++i; continue;
		}
		source_locations[line++]=(SourceLocation){filename,original++,start,i,0};
		++i;
	}
	if (line<=source_location_count)
		source_locations[line]=(SourceLocation){filename,original,len,len,0};
}

void still_diag_location(int expanded_line,const char **filename,int *source_line) {
	*source_line=expanded_line;
	if (expanded_line<1 || expanded_line>source_location_count || !source_locations) return;
	SourceLocation location=source_locations[expanded_line];
	if (location.filename) { *filename=location.filename; *source_line=location.original_line; }
}

char *still_diag_line(int line_num) {
	if (!source_text || line_num<1 || line_num>source_location_count)
		return calloc(1,1);
	const char *p=source_text+source_locations[line_num].start;
	const char *e=source_text+source_locations[line_num].end;
	while (e > p && (e[-1] == '\r'))
		e--;
	char *out = malloc((size_t)(e - p) + 1);
	memcpy(out, p, (size_t)(e - p));
	out[e - p] = '\0';
	return out;
}
void still_diag_offset_location(size_t offset,int *expanded_line,int *byte_column) {
	int low=1,high=source_location_count;
	if (!source_locations || offset>(size_t)source_len) { *expanded_line=0; *byte_column=1; return; }
	while (low<high) {
		int mid=low+(high-low+1)/2;
		if ((size_t)source_locations[mid].start<=offset) low=mid; else high=mid-1;
	}
	*expanded_line=low; *byte_column=(int)(offset-source_locations[low].start)+1;
}

int still_diag_error_count(void) { return timbr_error_count; }
int still_diag_warn_count(void) { return timbr_warning_count; }

static void vappend(char **buf, const char *fmt, va_list ap) {
	char tmp[512];
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	size_t old = *buf ? strlen(*buf) : 0;
	*buf = realloc(*buf, old + strlen(tmp) + 1);
	if (!*buf)
		return;
	memcpy(*buf + old, tmp, strlen(tmp) + 1);
}

void still_diag_help(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vappend(&pending_help, fmt, ap);
	va_end(ap);
}

void still_diag_note(const char *fmt, ...) {
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
		snprintf(code_buf, sizeof(code_buf), "%c%04d", level==TIMBR_WARN ? 'W' : 'E', code);
		code_str = code_buf;
	}

	still_diag_location(line_num,&filename,&line_num);
	if (json_diagnostics) {
		FILE *out=timbr_config.output_stream ? timbr_config.output_stream : stderr;
		if (level==TIMBR_ERROR) ++timbr_error_count;
		else ++timbr_warning_count;
		fputs("{\"type\":\"diagnostic\",\"level\":",out); json_string(out,level==TIMBR_ERROR ? "error" : "warning");
		fputs(",\"code\":",out); json_string(out,code_str);
		fputs(",\"message\":",out); json_string(out,title);
		fputs(",\"location\":{\"file\":",out); json_string(out,filename ? filename : "<wky>");
		fprintf(out,",\"line\":%d,\"column\":%d,\"column_unit\":\"byte\",\"length\":%d,\"source_line\":",line_num,col_num,len>0 ? len : 1);
		json_string(out,code_line); fputs("},\"notes\":[",out);
		if (pending_note) json_string(out,pending_note);
		fputs("],\"help\":[",out); if (pending_help) json_string(out,pending_help);
		fputs("]}\n",out);
		free(pending_help); free(pending_note); pending_help=pending_note=NULL;
		return;
	}
	TimbrSpan span = {.code_line = code_line,
					  .filename = filename ? filename : "<wky>",
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

void still_diag_error(int code, const char *filename, const char *code_line,
				 int line_num, int col_num, int len, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_ERROR, code, filename, code_line, line_num, col_num, len, fmt,
		 ap);
	va_end(ap);
}

void still_diag_error_at(int code, const char *filename, const char *code_line,
					int line_num, const char *fmt, ...) {
	char *owned = NULL;
	if (!code_line)
		code_line = owned = still_diag_line(line_num);
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_ERROR, code, filename, code_line, line_num, 1, 1, fmt, ap);
	va_end(ap);
	free(owned);
}

void still_diag_warn_at(int code, const char *filename, const char *code_line,
				   int line_num, const char *fmt, ...) {
	char *owned = NULL;
	if (!code_line)
		code_line = owned = still_diag_line(line_num);
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_WARN, code, filename, code_line, line_num, 1, 1, fmt, ap);
	va_end(ap);
	free(owned);
}

static void emit_node(int level,int code,const char *filename,const ASTNode *node,const char *fmt,va_list ap) {
	int line=node ? node->line : 0;
	char *source=still_diag_line(line);
	emit(level,code,filename,source,line,node && node->column>0 ? node->column : 1,
		node && node->span_length>0 ? node->span_length : 1,fmt,ap);
	free(source);
}
void still_diag_error_node(int code,const char *filename,const ASTNode *node,const char *fmt,...) {
	va_list ap; va_start(ap,fmt); emit_node(TIMBR_ERROR,code,filename,node,fmt,ap); va_end(ap);
}
void still_diag_warn_node(int code,const char *filename,const ASTNode *node,const char *fmt,...) {
	va_list ap; va_start(ap,fmt); emit_node(TIMBR_WARN,code,filename,node,fmt,ap); va_end(ap);
}

// First whole-word occurrence of `name` on the line: returns 1-based col
// and its length. Whole-word = bounded by non-identifier chars so `count`
// never matches inside `recount`.
static int find_name_col(const char *line, const char *name, int *out_len) {
	*out_len = 1;
	if (!line || !name || !*name)
		return 1;
	size_t nl = strlen(name);
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
				return (int)(p-line)+1;
			}
		}
	}
	return 1;
}

static void emit_named(int level, int code, const char *filename,
					   int line_num, const char *name, const char *fmt,
					   va_list ap) {
	char *code_line = still_diag_line(line_num);
	int len;
	int col = find_name_col(code_line, name, &len);
	emit(level, code, filename, code_line, line_num, col, len, fmt, ap);
	free(code_line);
}

void still_diag_error_named(int code, const char *filename, int line_num,
					   const char *name, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	emit_named(TIMBR_ERROR, code, filename, line_num, name, fmt, ap);
	va_end(ap);
}

void still_diag_warn(int code, const char *filename, const char *code_line,
				int line_num, int col_num, int len, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	emit(TIMBR_WARN, code, filename, code_line, line_num, col_num, len, fmt,
		 ap);
	va_end(ap);
}

void still_diag_summary(void) {
	if (json_diagnostics) return;
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

int still_diag_explain(const char *code) {
	static const char *explanations[]={NULL,
		"Syntax is invalid. Check the highlighted token and the surrounding delimiters.",
		"A name is unavailable in this scope. Check spelling, declaration order, imports, and visibility.",
		"Types do not match. Numeric conversions are checked; owners cannot be copied or forged. Use an explicit borrow, move, clone, or lossy conversion when appropriate.",
		"The argument count does not match the function signature.",
		"Arguments are invalid. Named arguments must identify distinct parameters; evaluation follows source order.",
		"A declaration or control transfer is invalid in this scope.",
		"A semantic constraint failed. Local addresses cannot outlive their storage, and pure functions cannot write externally or call unverified effects.",
		"This type has no field with the requested name.",
		"A declared effect contract failed. noalloc and nocapture are verified transitively before optimization; unknown external effects cannot establish a proof.",
		"An owner was consumed on this or another possible control-flow path. Borrow with ref_of, transfer once with move, or explicitly clone an independent owner.",
		"This statement is unreachable because control already left its block.",
		"A local binding is unused. Remove it or use a name beginning with an underscore."};
	char *end;
	long number=(code && (code[0]=='E' || code[0]=='W')) ? strtol(code+1,&end,10) : 0;
	if (number<1 || number>12 || *end || strlen(code)!=5 || (number>=11)!=(code[0]=='W')) {
		fprintf(stderr,"still: unknown diagnostic code '%s'\n",code ? code : ""); return 2;
	}
	if (json_diagnostics) {
		fputs("{\"code\":",stdout); json_string(stdout,code);
		fputs(",\"explanation\":",stdout); json_string(stdout,explanations[number]); fputs("}\n",stdout);
	} else printf("%s: %s\n",code,explanations[number]);
	return 0;
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

const char *still_diag_closest(const char *needle, const char *const *candidates) {
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
