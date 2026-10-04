#include <arena.h>
#include <codegen.h>
#include <diag.h>
#include <driver.h>
#include <lexer.h>
#include <parser.h>
#include <timbr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>
extern char **environ;

static int link_object(const char *output,int lto,int profile,const char *target) {
	char *args[12]; int count=0;
	args[count++]="clang"; args[count++]="-pthread";
#ifdef __APPLE__
	args[count++]="-target"; args[count++]=(char *)target;
#endif
	if (lto) args[count++]="-flto";
	if (profile) args[count++]="-fprofile-instr-generate";
	args[count++]="output.o"; args[count++]="-o"; args[count++]=(char *)output; args[count]=NULL;
	pid_t child;
	int result=posix_spawnp(&child,args[0],NULL,NULL,args,environ);
	if (result==ENOENT) { args[0]="cc"; result=posix_spawnp(&child,args[0],NULL,NULL,args,environ); }
	if (result) { fprintf(stderr,"kawac: cannot start linker: %s\n",strerror(result)); return 2; }
	int status;
	while (waitpid(child,&status,0)<0) if (errno!=EINTR) return 2;
	return WIFEXITED(status) && WEXITSTATUS(status)==0 ? 0 : 2;
}

// Exit-code contract (uniform across the toolchain):
//   0  success
//   1  compilation failed -- any diagnostic from the parser or codegen
//   2  kawac itself failed -- usage error, unreadable file, link failure
static void usage(const char *prog) {
	printf("Usage: %s [options] <source.kawa>\n"
		   "\n"
		   "Options:\n"
		   "  -o <name>    output executable name (default: source stem)\n"
		   "  -c           compile to output.bc only, don't link\n"
		   "  --debug/-g   runtime bounds checks (trap on violation)\n"
		   "  --test       run #[test] functions instead of main\n"
		   "  -O0/-O1/-O2/-O3  optimization level passed through (default -O2)\n"
		   "  --lto        enable link-time optimization (LTO)\n"
		   "  --pgo-gen[=file]  instrument binary for profile generation\n"
		   "  --pgo-use=<file>  use profile data for optimization\n"
		   "  --bounds-check=<safe|always|never> bounds checking policy\n"
		   "  --emit-hash  emit deterministic SHA-256 hash of output module\n"
		   "  --check      validate without writing artifacts or invoking the linker\n"
		   "  --memory-metrics  instrument managed reference checks and stable guards\n"
		   "  --diagnostic-format=<text|json>  diagnostic output format\n"
		   "  --explain <E####|W####>  explain a stable diagnostic code\n"
		   "  --format     validate syntax and write canonical source to stdout\n"
		   "  --format-check  fail if source needs formatting\n"
		   "  --color=<when>   diagnostics color: auto|always|never (default auto)\n"
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
	int enable_lto = 0;
	const char *pgo_gen = NULL;
	const char *pgo_use = NULL;
	int bounds_check_mode = 2; // safe by default; 1 = always, -1 = never
	int emit_hash = 0;
	int check_only = 0;
	int memory_metrics = 0;
	int json_diagnostics=0;
	const char *explain=NULL;
	int format=0, format_check=0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			out_name = argv[++i];
		} else if (strcmp(argv[i], "-c") == 0) {
			link_exe = 0;
		} else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-g") == 0) {
			debug_build = 1;
		} else if (strcmp(argv[i], "--test") == 0) {
			test_mode = 1;
		} else if (strcmp(argv[i], "--lto") == 0) {
			enable_lto = 1;
		} else if (strncmp(argv[i], "--pgo-gen", 9) == 0) {
			if (argv[i][9] == '=')
				pgo_gen = &argv[i][10];
			else
				pgo_gen = "default.profraw";
		} else if (strncmp(argv[i], "--pgo-use", 9) == 0) {
			if (argv[i][9] == '=')
				pgo_use = &argv[i][10];
			else if (i + 1 < argc && argv[i + 1][0] != '-')
				pgo_use = argv[++i];
		} else if (strncmp(argv[i], "--bounds-check=", 15) == 0) {
			const char *bm = &argv[i][15];
			if (strcmp(bm, "always") == 0)
				bounds_check_mode = 1;
			else if (strcmp(bm, "safe") == 0)
				bounds_check_mode = 2;
			else if (strcmp(bm, "never") == 0)
				bounds_check_mode = -1;
		} else if (strcmp(argv[i], "--emit-hash") == 0) {
			emit_hash = 1;
		} else if (strcmp(argv[i], "--check") == 0) {
			check_only = 1;
		} else if (strcmp(argv[i], "--memory-metrics") == 0) {
			memory_metrics = 1;
		} else if (!strcmp(argv[i],"--diagnostic-format=json")) {
			json_diagnostics=1;
		} else if (!strcmp(argv[i],"--diagnostic-format=text")) {
			json_diagnostics=0;
		} else if (!strcmp(argv[i],"--explain") && i+1<argc) {
			explain=argv[++i];
		} else if (!strcmp(argv[i],"--format") || !strcmp(argv[i],"--fmt")) {
			format=1;
		} else if (!strcmp(argv[i],"--format-check")) {
			format=1; format_check=1;
		} else if (strcmp(argv[i], "--color=always") == 0 ||
				   strcmp(argv[i], "--color=auto") == 0) {
			timbr_color_override =
				strcmp(argv[i], "--color=always") == 0 ? 1 : 0;
		} else if (strcmp(argv[i], "--color=never") == 0) {
			timbr_color_override = -1;
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

	kdiag_set_json(json_diagnostics);
	if (explain) return kdiag_explain(explain);
	if (!src_path) {
		usage(argv[0]);
		return 2;
	}

	timbr_init();
	// One summary line per process, on every exit path (parser errors exit
	// from main; codegen errors call exit(1) deep inside emission).
	atexit(kdiag_summary);

	Arena a;
	arena_init(&a, 1024 * 1024 * 10);

	char *expanded = expand_imports(src_path, &a);
	kdiag_set_source(expanded,(int)strlen(expanded));

	Lexer lex;
	// src_path is const (argv); the lexer wants a char* it never mutates.
	char *owned_path = arena_strdup(&a, src_path);
	lexer_init(&lex, expanded, &a, owned_path); // expanded is malloc'd, lexer treats it as read-only

	Parser p;
	parser_init(&p, &lex, &a);

	ASTNode *root = parse_program(&p);

	if (p.had_error)
		exit(1); // atexit hook prints the summary
	if (format) {
		char *original=read_file(src_path);
		char *formatted=format_source(original,src_path,&a);
		if (!formatted) return 1;
		int needs_format=strcmp(original,formatted)!=0;
		if (!format_check) fputs(formatted,stdout);
		free(original); free(formatted); free(expanded); arena_free(&a);
		return format_check && needs_format ? 1 : 0;
	}

	KawaCompiler kc;
	kawa_init(&kc, "kawa_main", &a);
	kc.test_mode = test_mode;
	kc.uses_print = p.uses_print;
	kawa_set_debug(&kc, debug_build);
	kawa_set_source_file(&kc, src_path);
	kc.enable_lto = enable_lto;
	kc.pgo_gen = pgo_gen;
	kc.pgo_use = pgo_use;
	kc.bounds_check_mode = bounds_check_mode;
	kc.emit_hash = emit_hash;
	kc.check_only = check_only;
	kc.memory_metrics = memory_metrics;
	// Codegen errors only know a line number; the source text lets them
	// render caret snippets like parser errors do.
	kc.source_text = expanded;
	kc.source_len = (int)strlen(expanded);
	kawa_compile(&kc, root);
	kc.opt_level = opt_level;
	kawa_optimize_and_write(&kc, "output.bc");
	if (check_only) return 0;

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

	int rc = link_object(out_name,enable_lto,pgo_gen!=NULL,LLVMGetTarget(kc.module));
	if (rc != 0) {
		fprintf(stderr, "kawac: linking failed\n");
		return 2;
	}
	printf("[Kawa] Built '%s'\n", out_name);

	return 0;
}
