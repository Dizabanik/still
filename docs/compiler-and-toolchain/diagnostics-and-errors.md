# Diagnostics and error codes

Whisky includes a diagnostic engine that generates error reports with source context snippets, caret underlines, and fuzzy symbol suggestions.

## Diagnostic report anatomy

A diagnostic report consists of:
1. Error or warning code: A unique, stable identifier (such as `error[E0002]`).
2. Source position: File path, line number, and column number formatted as `path:line:col`.
3. Code context: The offending source code line with caret pointers (`^~~~`).
4. Hint: Correction advice or fuzzy match suggestions based on edit distance.

### Example output

```
error[E0002]: cannot find variable `recvd` in this scope
  --> server.wky:14:9
   |
14 |     let r = recvd;
   |             ^^^^^
   |
   = help: a local variable with a similar name exists: `recv_val`
```

## Error code catalog

| Code | Category | Official explanation |
|:---|:---|:---|
| `E0001` | Syntax | Syntax is invalid. Check the highlighted token and the surrounding delimiters. |
| `E0002` | Scope | A name is unavailable in this scope. Check spelling, declaration order, imports, and visibility. |
| `E0003` | Type | Types do not match. Numeric conversions are checked; owners cannot be copied or forged. Use an explicit borrow, move, clone, or lossy conversion when appropriate. |
| `E0004` | Arity | The argument count does not match the function signature. |
| `E0005` | Arguments | Arguments are invalid. Named arguments must identify distinct parameters; evaluation follows source order. |
| `E0006` | Scope | A declaration or control transfer is invalid in this scope. |
| `E0007` | Semantic | A semantic constraint failed. Local addresses cannot outlive their storage, and pure functions cannot write externally or call unverified effects. |
| `E0008` | Struct field | This type has no field with the requested name. |
| `E0009` | Effect | A declared effect contract failed. noalloc and nocapture are verified transitively before optimization; unknown external effects cannot establish a proof. |
| `E0010` | Ownership | An owner was consumed on this or another possible control-flow path. Borrow with ref_of, transfer once with move, or explicitly clone an independent owner. |

## Warning code catalog

| Code | Category | Official explanation |
|:---|:---|:---|
| `W0011` | Reachability | This statement is unreachable because control already left its block. |
| `W0012` | Unused binding | A local binding is unused. Remove it or use a name beginning with an underscore. |

## Querying diagnostic explanations

You can inspect the explanation for any diagnostic code directly from the command line:

```sh
still --explain E0002
```

Output:
```
E0002: A name is unavailable in this scope. Check spelling, declaration order, imports, and visibility.
```

To emit machine-readable JSON explanations, pass `--diagnostic-format=json`:

```sh
still --diagnostic-format=json --explain E0002
```

Output:
```json
{"code":"E0002","explanation":"A name is unavailable in this scope. Check spelling, declaration order, imports, and visibility."}
```

## Fuzzy symbol matching

When an undefined identifier or field name is parsed, the compiler checks Levenshtein edit distance against candidate names visible in the current lexical scope. If the edit distance is within tolerance, a suggestion is printed with the error:

```
= help: a field with a similar name exists: `second`
```

## Terminal color settings

Diagnostics detect terminal capabilities automatically:
* If the terminal supports ANSI escape sequences, color output is enabled by default.
* If the `NO_COLOR` environment variable is present, color output is disabled.
* Color behavior can be set explicitly using `--color=always`, `--color=never`, or `--color=auto`.
