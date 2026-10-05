// still diagnostics: one front door for every message the compiler emits.
//
// Style contract (rustc/clang/Zig/Go blend):
//   - errors carry stable codes (E####) so docs and greps can reference them
//   - primary label rides the caret line; notes/help render as sub-lines
//   - warnings never fail the build; errors do
//   - "did you mean" suggestions come from a small Levenshtein over the
//     identifiers in scope at the emit site
//   - exit codes stay 0 / 1 / 2 (see main.c)
#ifndef STILL_DIAG_H
#define STILL_DIAG_H

#include <timbr.h>
#include <stdio.h>

// Stable diagnostic codes.
enum {
	STILL_E_PARSE = 1,	 // E0001 syntax error
	STILL_E_UNDEF,		 // E0002 unknown name (fn/var/type/field)
	STILL_E_TYPE,		 // E0003 type mismatch
	STILL_E_ARITY,		 // E0004 wrong argument count
	STILL_E_ARGS,		 // E0005 invalid arguments
	STILL_E_SCOPE,		 // E0006 duplicate declaration
	STILL_E_SEMANTIC,	 // E0007 other semantic constraint
	STILL_E_MEMBER = 8,    // E0008 unknown field
	STILL_E_EFFECT = 9,    // E0009 invalid effect contract
	STILL_E_OWNERSHIP = 10, // E0010 affine ownership violation
	STILL_W_UNREACHABLE = 11,	 // W0011 code after return is unreachable
	STILL_W_UNUSED,		 // W0012 unused local
};

// Emit an error with a source span. fmt is the title; span describes the
// highlighted region. Returns nothing -- callers decide control flow.
void still_diag_error(int code, const char *filename, const char *code_line,
				 int line_num, int col_num, int len, const char *fmt, ...);

// Same shape but a warning.
void still_diag_warn(int code, const char *filename, const char *code_line,
				int line_num, int col_num, int len, const char *fmt, ...);

// Attach a help/note to the NEXT emitted diagnostic (one-shot buffers).
void still_diag_help(const char *fmt, ...);
void still_diag_note(const char *fmt, ...);

// "did you mean `x`?" helper: formats into the pending-help slot when best
// candidate distance <= 2, silently no-ops otherwise. candidates is a NULL-
// terminated array of names.
const char *still_diag_closest(const char *needle, const char *const *candidates);

// Summary line after a phase: "error: could not compile X due to N previous
// errors" style. no-op when zero errors.
void still_diag_summary(void);

int still_diag_error_count(void);
int still_diag_warn_count(void);

// Point the diag layer at the program's source text so codegen-side errors
// (which only know a line NUMBER off the AST node) can still render the
// offending source line. Call once from the driver before compiling.
void still_diag_set_source(const char *text, int len);
void still_diag_set_json(int enabled);
void still_diag_json_string(FILE *out,const char *text);
int still_diag_explain(const char *code);
void still_diag_location(int expanded_line, const char **filename, int *source_line);
void still_diag_offset_location(size_t offset, int *expanded_line, int *byte_column);
struct ASTNode;
void still_diag_error_node(int code,const char *filename,const struct ASTNode *node,const char *fmt,...);
void still_diag_warn_node(int code,const char *filename,const struct ASTNode *node,const char *fmt,...);

// Copy of source line `line_num` (1-based), tabs preserved. Returns a
// malloc'd string; "" for out-of-range/unknown lines. Caller frees.
char *still_diag_line(int line_num);

#endif

// Codegen-side emitters: the AST node supplies the line, still_diag_line() pulls
// the source text. col is 1-based (0/1 = start of line), len 0 = 1 char.
void still_diag_error_at(int code, const char *filename, const char *code_line,
					int line_num, const char *fmt, ...);
void still_diag_warn_at(int code, const char *filename, const char *code_line,
				   int line_num, const char *fmt, ...);

// Like *_at but the span anchors to the FIRST whole-word occurrence of
// `name` on the line (falls back to column 1). Perfect for "cannot find X"
// diagnostics where only the AST line number is known.
void still_diag_error_named(int code, const char *filename, int line_num,
					   const char *name, const char *fmt, ...);
