# Indexed scanning

The existing owning API remains available, including `RawToken::lexeme`:

```cpp
Scanner scanner(source, config);
Vector<RawToken> tokens = scanner.scan();
// tokens[i].lexeme owns its text, with the existing decoding rules.
```

Default construction selects the new indexed API:

```cpp
Scanner scanner;
scanner.commentCfg = config;

String source = "name = 42";
Vector<IndexedToken> tokens = scanner.stream(source);

std::string_view name = tokens[1].text(source);
String ownedName(name); // optional copy

// Equivalent manual access:
String alsoName = source.substr(tokens[1].offset, tokens[1].length);

// Reuse the scanner with another complete source:
auto nextTokens = scanner.stream("other = 7");
```

`stream()` accepts `std::string_view`, including views of non-null-terminated
buffers and strings containing NUL bytes. It processes the entire buffer
synchronously; it is not a chunked input or lazy iteration interface.
It uses the current `commentCfg` and resets scanning state on every call,
including calls after a scanning error. Only default-constructed scanners
accept `stream()`; source-taking constructors continue to use `scan()`.
Using the wrong method throws `std::logic_error`.

Neither indexed tokens nor the scanner retain the supplied source after
`stream()` returns. The caller owns its storage. Tokens contain byte offsets
and lengths, so they can be used with a relocated buffer containing identical
bytes and can outlive the scanner. A view returned by `text(source)` borrows
the buffer passed to that particular call. Keep that buffer alive and stable
while using the view. `text()` checks range bounds but cannot detect a different
source with the same length.

`IndexedToken` orders its fields as `offset`, `length`, `line`, `column`,
`aux`, `kind`, and `ppKind` to minimize alignment padding without narrowing
their types. Manual aggregate initializers must follow that declaration order:

```cpp
IndexedToken token{.offset = 0, .length = 4, .kind = RawKind::Identifier};
```

Indexed tokens preserve source spelling. This differs intentionally from
`RawToken::lexeme`:

| Source | Indexed text | Owning lexeme |
| --- | --- | --- |
| `"a\nb"` | Quotes and backslash retained | Quotes removed, escape decoded |
| `++` | `++`, with `aux == 2` when collapsing | `+`, with `aux == 2` |
| A run of tabs | The original tabs | Four spaces, with tab count in `aux` |
| `# if` (configured directive) | `# if` | `#if` |
| A nested comment body | Includes inner delimiters | Inner delimiters removed |

SOF and EOF have empty ranges at byte zero and the end of the buffer.
Quotes still follow `QuoteConfig` and prefix rules to find their boundaries
and validate escapes; streaming avoids building the decoded text. Comments,
text blocks, and preprocessor directives follow the same configuration as
`scan()`. Indented text-block newline ranges can include whitespace consumed
between text lines. Tokens are not a guarantee of complete source coverage:
for example, a BOM and skipped comments are omitted.

`stream()` returns a separate `Vector<IndexedToken>` and does not populate
`rawTokens`. Existing `Structurizer::structurize()` still accepts
`Vector<RawToken>`; retain the owning pipeline for those callers during migration.

Build with `LEX_BUILD_BENCHMARKS=ON` and `CMAKE_BUILD_TYPE=Release` to compare
the APIs using `lex_benchmark`. Its `stream_ms` column measures repeated calls
on a configured scanner; `scan_ms` includes construction of an owning scanner.
