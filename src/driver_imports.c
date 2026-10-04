#include "driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

/* Decode internal file markers identically in the lexer and diagnostics.
 * Paths can contain quotes, backslashes, control bytes and UTF-8. */
static int hex_digit(unsigned char ch) {
	if (ch>='0' && ch<='9') return ch-'0';
	if (ch>='a' && ch<='f') return ch-'a'+10;
	if (ch>='A' && ch<='F') return ch-'A'+10;
	return -1;
}
static char *quoted_path(const char **cursor,const char *end) {
	const char *p=*cursor;
	if (p>=end || *p++!='"') return NULL;
	char *path=malloc((size_t)(end-p)+1); size_t length=0;
	if (!path) { fputs("kawac: out of memory\n",stderr); exit(2); }
	while (p<end && *p!='"') {
		unsigned char ch=(unsigned char)*p++;
		if (ch=='\\') {
			if (p==end) { free(path); return NULL; }
			ch=(unsigned char)*p++;
			if (ch=='n') ch='\n';
			else if (ch=='r') ch='\r';
			else if (ch=='t') ch='\t';
			else if (ch=='x') {
				if (end-p<2 || hex_digit(p[0])<0 || hex_digit(p[1])<0) { free(path); return NULL; }
				ch=(unsigned char)(hex_digit(p[0])*16+hex_digit(p[1])); p+=2;
			} else if (ch!='\\' && ch!='"' && ch!='\'') { free(path); return NULL; }
		}
		if (!ch) { free(path); return NULL; }
		path[length++]=(char)ch;
	}
	if (p==end || *p++!='"') { free(path); return NULL; }
	path[length]=0; *cursor=p; return path;
}
int driver_module_location(const char *text,size_t length,char **filename,int *line) {
	if (length<10 || strncmp(text,"#module \"",9)) return 0;
	const char *p=text+8, *end=text+length;
	char *path=quoted_path(&p,end);
	if (!path) return 0;
	while (p<end && (*p==' ' || *p=='\t')) ++p;
	int number=1;
	if (p<end) {
		number=0;
		for (; p<end && isdigit((unsigned char)*p); ++p) {
			if (number>(INT_MAX-9)/10) { free(path); return 0; }
			number=number*10+(*p-'0');
		}
		while (p<end && (*p==' ' || *p=='\t' || *p=='\r')) ++p;
		if (p!=end || number<1) { free(path); return 0; }
	}
	*filename=path; *line=number; return 1;
}

char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "kawac: cannot open '%s'\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(len + 1);
	if (fread(buf, 1, len, f) != (size_t)len) {
		fprintf(stderr, "kawac: short read on '%s'\n", path);
		exit(2);
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

// --- Multi-file imports ---------------------------------------------------
// `import "dir/file.kawa";` splices the named file's text in place of the
// import statement before lexing. Paths are relative to the importing
// file's directory; each file expands at most once (first use wins), so
// diamond includes are safe and cycles terminate.

#define MAX_IMPORT_FILES 256

typedef struct ImportCtx {
	char *paths[MAX_IMPORT_FILES]; // canonical-ish keys already expanded
	int count;
} ImportCtx;

static int is_ident_char(char ch) {
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		   (ch >= '0' && ch <= '9') || ch == '_' || ch == '$';
}

static void append_char(char **out, size_t *len, size_t *cap, char ch) {
	if (*len + 1 >= *cap) {
		*cap = *cap ? *cap * 2 : 4096;
		*out = realloc(*out, *cap);
		if (!*out) {
			fprintf(stderr, "kawac: out of memory\n");
			exit(2);
		}
	}
	(*out)[(*len)++] = ch;
}

static void append_str(char **out, size_t *len, size_t *cap, const char *s) {
	for (; *s; s++)
		append_char(out, len, cap, *s);
}
static void append_marker(char **out,size_t *len,size_t *cap,const char *path,int line) {
	append_str(out,len,cap,"\n#module \"");
	for (const unsigned char *p=(const unsigned char *)path; *p; ++p) {
		if (*p=='"' || *p=='\\') append_char(out,len,cap,'\\');
		if (*p<32) {
			char escape[5]; snprintf(escape,sizeof(escape),"\\x%02x",*p); append_str(out,len,cap,escape);
		} else append_char(out,len,cap,(char)*p);
	}
	char tail[32]; snprintf(tail,sizeof(tail),"\" %d\n",line); append_str(out,len,cap,tail);
}

static char *dir_name(const char *path, Arena *a) {
	const char *slash = strrchr(path, '/');
	if (!slash)
		return arena_strdup(a, ".");
	size_t len = slash - path;
	if (len == 0)
		len = 1; // "/foo.kawa" -> "/"
	char *out = arena_alloc(a, len + 1);
	memcpy(out, path, len);
	out[len] = '\0';
	return out;
}

static char *join_path(const char *dir, const char *rel, Arena *a) {
	if (rel[0] == '/')
		return arena_strdup(a, rel);
	size_t dl = strlen(dir), rl = strlen(rel);
	char *out = arena_alloc(a, dl + rl + 2);
	memcpy(out, dir, dl);
	out[dl] = '/';
	memcpy(out + dl + 1, rel, rl + 1);
	return out;
}

static void expand_file(ImportCtx *ctx, const char *abs_path, Arena *a,
						char **out, size_t *out_len, size_t *out_cap);

// Expand one file: read it, then walk its text splicing in any quoted
// imports. Output accumulates into *out (grown as needed).
static void expand_text(ImportCtx *ctx, const char *src_dir, const char *src,
						const char *current_file,
						Arena *a, char **out, size_t *out_len,
						size_t *out_cap) {
	const char *p = src;
	int original_line=1;
	while (*p) {
		// Imports inside comments and literals are text, never dependencies.
		if ((p[0]=='/' && p[1]=='/') || (p[0]=='#' && p[1]!='[')) {
			while (*p && *p!='\n') append_char(out,out_len,out_cap,*p++);
			continue;
		}
		if (*p=='"' || *p=='\'') {
			char quote=*p; append_char(out,out_len,out_cap,*p++);
			while (*p) {
				char ch=*p++; append_char(out,out_len,out_cap,ch);
				if (ch=='\n') ++original_line;
				if (ch==quote) break;
				if (ch=='\\' && *p) {
					if (*p=='\n') ++original_line;
					append_char(out,out_len,out_cap,*p++);
				}
			}
			continue;
		}
		if (p[0] == 'i' && strncmp(p, "import", 6) == 0 &&
			(p == src || !is_ident_char(p[-1])) && !is_ident_char(p[6])) {
			const char *q = p + 6;
			while (*q == ' ' || *q == '\t')
				q++;
			if (*q == '"') {
				// Quoted import: find the closing quote and the ';'.
				const char *after=q;
				const char *limit=q;
				while (*limit && *limit!='\n') ++limit;
				char *rel=quoted_path(&after,limit);
				if (rel) {
					const char *semi = after;
					while (*semi == ' ' || *semi == '\t')
						semi++;
					if (*semi == ';') {
						char *abs = join_path(src_dir, rel, a);
						free(rel);
						expand_file(ctx, abs, a, out, out_len, out_cap);
						append_marker(out,out_len,out_cap,current_file,original_line);
						const char *line_start=p;
						while (line_start>src && line_start[-1]!='\n') --line_start;
						for (const char *padding=line_start; padding<=semi; ++padding)
							append_char(out,out_len,out_cap,' ');

						p = semi + 1; // skip past `import "...";`
						continue;
					}
					free(rel);
				}
			}
		}
		// Not an import statement: copy the character through.
		if (*p=='\n') ++original_line;
		append_char(out, out_len, out_cap, *p);
		p++;
	}
}

static void expand_file(ImportCtx *ctx, const char *abs_path, Arena *a,
						char **out, size_t *out_len, size_t *out_cap) {
	char canonical[PATH_MAX];
	if (realpath(abs_path,canonical)) abs_path=arena_strdup(a,canonical);
	// One expansion per file, first use wins.
	for (int i = 0; i < ctx->count; i++) {
		if (strcmp(ctx->paths[i], abs_path) == 0)
			return;
	}
	if (ctx->count >= MAX_IMPORT_FILES) {
		fprintf(stderr, "kawac: too many imported files (max %d)\n",
				MAX_IMPORT_FILES);
		exit(2);
	}
	ctx->paths[ctx->count++] = arena_strdup(a, abs_path);

	char *src = read_file(abs_path);
	append_marker(out,out_len,out_cap,abs_path,1);

	char *src_dir = dir_name(abs_path, a);
	expand_text(ctx, src_dir, src, abs_path, a, out, out_len, out_cap);
	free(src);
}

// Entry: returns fully-expanded source text (malloc'd, NUL-terminated).
char *expand_imports(const char *entry_path, Arena *a) {
	ImportCtx ctx = {0};
	size_t cap = 1 << 16, len = 0;
	char *out = malloc(cap);
	out[0] = '\0';
	expand_file(&ctx, entry_path, a, &out, &len, &cap);
	out[len] = '\0';
	return out;
}
