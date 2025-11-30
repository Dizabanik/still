#include <arena.h>
#include <codegen.h>
#include <lexer.h>
#include <parser.h>
#include <stdio.h>
#include <stdlib.h>

char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror("Could not open file");
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(len + 1);
	fread(buf, 1, len, f);
	buf[len] = '\0';
	fclose(f);
	return buf;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: %s <source.kawa>\n", argv[0]);
		return 1;
	}

	Arena a;
	arena_init(&a, 1024 * 1024 * 10);

	char *src = read_file(argv[1]);

	Lexer lex;
	lexer_init(&lex, src, &a, argv[1]);

	Parser p;
	parser_init(&p, &lex, &a);

	printf("[Kawa] Parsing...\n");
	ASTNode *root = parse_program(&p);

	if (p.had_error) {
		fprintf(stderr, "[Kawa] Aborting due to parse errors.\n");
		exit(1);
	}

	printf("[Kawa] Compiling to LLVM IR...\n");
	KawaCompiler kc;
	kawa_init(&kc, "kawa_main");
	kawa_compile(&kc, root);

	printf("[Kawa] Optimizing & Writing 'output.bc'...\n");
	kawa_optimize_and_write(&kc, "output.bc");

	printf("[Kawa] Done. Use 'lli output.bc' to run.\n");

	arena_free(&a);
	free(src);
	return 0;
}
