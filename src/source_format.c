#include "driver.h"
#include "lexer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Token-preserving canonical formatting. The driver validates syntax first.
 * Raw literal spellings and comment text survive byte for byte; whitespace
 * separates tokens explicitly so formatting cannot accidentally fuse them. */
typedef struct { char *text; size_t length, capacity; unsigned indent; int line_start; } Output;
static void append(Output *out,const char *text,size_t length) {
    if (length>SIZE_MAX-out->length-1) { fputs("kawac: formatted source is too large\n",stderr); exit(2); }
    size_t need=out->length+length+1;
    if (need>out->capacity) {
        size_t capacity=out->capacity ? out->capacity : 4096;
        while (capacity<need) { if (capacity>SIZE_MAX/2) { capacity=need; break; } capacity*=2; }
        char *grown=realloc(out->text,capacity);
        if (!grown) { fputs("kawac: out of memory while formatting\n",stderr); exit(2); }
        out->text=grown; out->capacity=capacity;
    }
    memcpy(out->text+out->length,text,length); out->length+=length; out->text[out->length]=0;
}
static void newline(Output *out) {
    while (out->length && out->text[out->length-1]==' ') --out->length;
    if (out->length && out->text[out->length-1]!='\n') append(out,"\n",1);
    out->line_start=1;
}
static void begin(Output *out) {
    if (!out->line_start) return;
    for (unsigned i=0; i<out->indent; ++i) append(out,"    ",4);
    out->line_start=0;
}
static void comments(Output *out,const char *source,size_t start,size_t end) {
    for (size_t i=start; i<end;) {
        if (isspace((unsigned char)source[i])) { ++i; continue; }
        if (source[i]=='#' || (source[i]=='/' && i+1<end && source[i+1]=='/')) {
            size_t from=i;
            while (i<end && source[i]!='\n' && source[i]!='\r') ++i;
            if (!out->line_start) append(out," ",1);
            begin(out); append(out,source+from,i-from); newline(out);
        } else ++i;
    }
}
char *format_source(char *source,const char *filename,Arena *arena) {
    Lexer lexer; lexer_init(&lexer,source,arena,(char *)filename);
    Output out={.line_start=1};
    size_t cursor=0; int parentheses=0;
    TokenType previous=TOK_EOF;
    for (;;) {
        Token token=lexer_next(&lexer);
        size_t start=token.type==TOK_EOF ? lexer.len : token.pos;
        comments(&out,source,cursor,start);
        if (token.type==TOK_EOF) break;
        if (token.type==TOK_ERROR) { free(out.text); return NULL; }
        if (token.type==TOK_RBRACE) { newline(&out); if (out.indent) --out.indent; }
        if (previous==TOK_RBRACE && token.type!=TOK_SEMICOLON && token.type!=TOK_COMMA &&
            token.type!=TOK_RPAREN && token.type!=TOK_RBRACKET && token.type!=TOK_DOT &&
            token.type!=TOK_ELSE && token.type!=TOK_DREGS) newline(&out);
        int tight=token.type==TOK_COMMA || token.type==TOK_SEMICOLON || token.type==TOK_COLON ||
            token.type==TOK_RPAREN || token.type==TOK_RBRACKET || token.type==TOK_DOT ||
            previous==TOK_LPAREN || previous==TOK_LBRACKET || previous==TOK_DOT;
        if (token.type==TOK_LPAREN && previous!=TOK_IF && previous!=TOK_WHILE && previous!=TOK_FOR &&
            previous!=TOK_SWITCH && previous!=TOK_MATCH && previous!=TOK_BATCH && previous!=TOK_DEFER) tight=1;
        if (token.type==TOK_LBRACKET) tight=1;
        if (!tight && !out.line_start) append(&out," ",1);
        begin(&out); append(&out,source+start,lexer.pos-start);
        if (token.type==TOK_LPAREN) ++parentheses;
        if (token.type==TOK_RPAREN) --parentheses;
        if (token.type==TOK_LBRACE) { ++out.indent; newline(&out); }
        if ((token.type==TOK_SEMICOLON && !parentheses) || token.type==TOK_ATTRIBUTE) newline(&out);
        previous=token.type; cursor=lexer.pos;
    }
    newline(&out);
    if (!out.text) out.text=calloc(1,1);
    return out.text;
}
