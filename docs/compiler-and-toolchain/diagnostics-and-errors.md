# Diagnostics & Error Codes

Whisky includes a multi-span diagnostic engine (`timbr`) that produces clear, rustc-style error messages with stable error codes, source code snippets, caret underlines, and "did you mean?" suggestions.

---

## 1. Diagnostic Anatomy

A standard error report from `still` consists of:
1. **Error Code**: A unique, stable identifier (e.g. `error[E0003]`).
2. **File & Location**: Line and column number formatted as `path:line:col`.
3. **Source Context**: The offending source line displayed with caret pointers (`^~~~`).
4. **Help / Hint**: Actionable correction advice or Levenshtein-distance fuzzy match suggestions.

### Example Output

```
error[E0002]: cannot find variable `recvd` in this scope
  --> server.wky:14:9
   |
14 |     let r = recvd;
   |             ^^^^^
   = help: a local variable with a similar name exists: `recv_val`
```

---

## 2. Error Code Index

| Code | Category | Description |
|:---|:---|:---|
| `E0001` | Syntax | Parser or token grammar violation (missing semicolon, unexpected token) |
| `E0002` | Scope | Undefined variable, function, or struct name |
| `E0003` | Type | Incompatible type assignment, binary operator operand mismatch |
| `E0004` | Arity | Call site provided incorrect number of arguments for function |
| `E0005` | Arguments | Argument type mismatch at call site |
| `E0006` | Scope | Re-declaration of an identifier within the same scope |
| `E0007` | Semantic | Invalid break/continue outside loops, invalid return statement |
| `E0008` | Struct | Unknown field name on struct access |
| `E0009` | Index | Attempted to index a non-indexable type |
| `E0010` | Match | Non-exhaustive match arms or invalid pattern payload |

---

## 3. Warning Code Index

| Code | Description |
|:---|:---|
| `W0011` | Unreachable code detected following a `return`, `exit`, or unconditional loop |
| `W0012` | Unused variable declared but never read |

---

## 4. Fuzzy Symbol Matching ("Did You Mean?")

When an undefined identifier or struct field is encountered, `still` computes Levenshtein edit distances against all identifiers visible in the current scope:

* If distance $\le 2$, a suggestion is automatically rendered:
  ```
  = help: a field with a similar name exists: `second`
  ```
* For functions, candidate names are collected across all imported modules and top-level definitions.

---

## 5. Controlling Color Output

Diagnostics automatically respect terminal capabilities and environment variables:
* If the terminal supports ANSI colors, colored output is enabled by default.
* If the `NO_COLOR` environment variable is set, colors are disabled.
* Override manually with `--color=always`, `--color=never`, or `--color=auto`.
