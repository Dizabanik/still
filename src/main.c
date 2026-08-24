#include <arena.h>
#include <codegen.h>
#include <lexer.h>
#include <parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Exit-code contract (uniform across the toolchain):
//   0  success
//   1  compilation failed -- any diagnostic from the parser or codegen
//   2  kawac itself failed -- usage error, unreadable file, link failure
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
						Arena *a, char **out, size_t *out_len,
						size_t *out_cap) {
	const char *p = src;
	while (*p) {
		if (p[0] == 'i' && strncmp(p, "import", 6) == 0 &&
			(p == src || !is_ident_char(p[-1]))) {
			const char *q = p + 6;
			while (*q == ' ' || *q == '\t')
				q++;
			if (*q == '"') {
				// Quoted import: find the closing quote and the ';'.
				const char *end = strchr(q + 1, '"');
				if (end) {
					const char *semi = end + 1;
					while (*semi == ' ' || *semi == '\t')
						semi++;
					if (*semi == ';') {
						size_t plen = end - (q + 1);
						char *rel = arena_alloc(a, plen + 1);
						memcpy(rel, q + 1, plen);
						rel[plen] = '\0';

						char *abs = join_path(src_dir, rel, a);
						expand_file(ctx, abs, a, out, out_len, out_cap);

						p = semi + 1; // skip past `import "...";`
						continue;
					}
				}
			}
		}
		// Not an import statement: copy the character through.
		append_char(out, out_len, out_cap, *p);
		p++;
	}
}

static void expand_file(ImportCtx *ctx, const char *abs_path, Arena *a,
						char **out, size_t *out_len, size_t *out_cap) {
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
	append_str(out, out_len, out_cap, "\n// ---- imported from \"");
	append_str(out, out_len, out_cap, abs_path);
	append_str(out, out_len, out_cap, "\" ----\n");

	char *src_dir = dir_name(abs_path, a);
	expand_text(ctx, src_dir, src, a, out, out_len, out_cap);
	free(src);
}

// Entry: returns fully-expanded source text (malloc'd, NUL-terminated).
static char *expand_imports(const char *entry_path, Arena *a) {
	ImportCtx ctx = {0};
	size_t cap = 1 << 16, len = 0;
	char *out = malloc(cap);
	out[0] = '\0';
	expand_file(&ctx, entry_path, a, &out, &len, &cap);
	out[len] = '\0';
	return out;
}

static void usage(const char *prog) {
	printf("Usage: %s [options] <source.kawa>\n"
		   "\n"
		   "Options:\n"
		   "  -o <name>    output executable name (default: source stem)\n"
		   "  -c           compile to output.bc only, don't link\n"
			   "  --debug/-g   runtime bounds checks (trap on violation)\n"
			   "  --test       run #[test] functions instead of main\n"
		   "  -O0/-O1/-O2/-O3  optimization level passed through (default -O2)\n"
		   "  --version    print version and exit\n"
		   "  -h, --help   show this help\n",
		   prog);
}

int main(int argc, char **argv) {
	const char *src_path = NULL;
	const char *out_name = NULL;
	int opt_level = 2;
	int link_exe = 1;
	int debug_build = 0;
	int test_mode = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			out_name = argv[++i];
		} else if (strcmp(argv[i], "-c") == 0) {
			link_exe = 0;
		} else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-g") == 0) {
			debug_build = 1;
		} else if (strcmp(argv[i], "--test") == 0) {
			test_mode = 1;
		} else if (strncmp(argv[i], "-O", 2) == 0 &&
				   argv[i][2] >= '0' && argv[i][2] <= '3' && !argv[i][3]) {
			opt_level = argv[i][2] - '0';
		} else if (strcmp(argv[i], "--version") == 0) {
			printf("kawac 0.1.0\n");
			return 0;
		} else if (strcmp(argv[i], "-h") == 0 ||
				   strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "kawac: unknown option '%s'\n", argv[i]);
			usage(argv[0]);
			return 2;
		} else {
			src_path = argv[i];
		}
	}

	if (!src_path) {
		usage(argv[0]);
		return 2;
	}

	Arena a;
	arena_init(&a, 1024 * 1024 * 10);

	char *expanded = expand_imports(src_path, &a);

	Lexer lex;
	// src_path is const (argv); the lexer wants a char* it never mutates.
	char *owned_path = arena_strdup(&a, src_path);
	lexer_init(&lex, expanded, &a, owned_path); // expanded is malloc'd, lexer treats it as read-only

	Parser p;
	parser_init(&p, &lex, &a);

	ASTNode *root = parse_program(&p);

	if (p.had_error) {
		fprintf(stderr, "[Kawa] Aborting due to parse errors.\n");
		exit(1);
	}

	KawaCompiler kc;
	kawa_init(&kc, "kawa_main", &a);
	kc.test_mode = test_mode;
	kawa_set_debug(&kc, debug_build);
	kawa_set_source_file(&kc, src_path);
	kawa_compile(&kc, root);
	kc.opt_level = opt_level;
	kawa_optimize_and_write(&kc, "output.bc");

	if (!link_exe) {
		printf("[Kawa] Wrote output.bc\n");
		return 0;
	}

	// Link a native executable via the system C compiler.
	if (!out_name) {
		// Default executable name: source stem.
		const char *stem_end = strrchr(src_path, '.');
		size_t slen = stem_end ? (size_t)(stem_end - src_path)
							   : strlen(src_path);
		char *def = arena_alloc(&a, slen + 1);
		memcpy(def, src_path, slen);
		def[slen] = '\0';
		out_name = def;
	}

	char cmd[4096];
	// Link the kawac-emitted object (output.o) rather than recompiling the
	// bitcode: keeps debug sections intact and skips redundant codegen.
	snprintf(cmd, sizeof(cmd),
			 "clang output.o -o '%s' 2>/dev/null || "
			 "cc output.o -o '%s'",
			 out_name, out_name);
	int rc = system(cmd);
	if (rc != 0) {
		fprintf(stderr, "kawac: linking failed\n");
		return 2;
	}
	printf("[Kawa] Built '%s'\n", out_name);

	return 0;
}
