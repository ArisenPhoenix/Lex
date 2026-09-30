// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <iomanip>

using std::uint8_t;
using String = std::string;

template <typename V>
using Vector = std::vector<V>;

enum class RawKind : uint8_t {
    SOF,
    EOF_,
    Newline,        // '\n'
    Tab,
    Space,
    Semicolon,      // ';'

    CommentLineStart,   // carries lexeme like "//"
    CommentBlockStart,  // carries lexeme like "/*"
    CommentBlockEnd,    // carries lexeme like "*/"
    Comment,
    Identifier,
    Number,
    String,
    Char,
    Text,

    Operator,       // single-char value; identical runs set aux (e.g. '+' aux=2). Compounds are a later pass.
    SpecialChar,    // Anything not clearly an identifier or Operator or Punctuation
    Punctuation,    // single-char value; identical runs set aux (e.g. ':' aux=2, '.' aux=3)
    Preprocessor,   // a recognized directive keyword (see PreprocessorConfig); classified via ppKind
    Unknown,
    NoOp,

    Indent,
    Dedent,

    // Text blocks (see TextBlockConfig): the opener, one TextLine per line
    // of raw text, and the closer when the block has one. Consecutive
    // TextLines belong together; the Newline tokens between them carry the
    // line breaks (aux counts blank lines).
    TextBlockStart,
    TextLine,
    TextBlockEnd,
};

// Classifies a RawKind::Preprocessor token. Lex stays language-agnostic: it
// only recognizes and buckets directive keywords a consumer configured (see
// PreprocessorConfig), it never evaluates conditions or decides which branch
// of an #if/#else is "real" - that policy belongs to the consumer (e.g. a
// C++-aware tool like DocWriter).
enum class PreprocessorKind : uint8_t {
    None,
    If,     // if / ifdef / ifndef, or whatever the consumer maps to "opens a conditional branch"
    Elif,
    Else,
    Endif,
    Other,  // recognized directive keyword that isn't part of conditional nesting (e.g. define, include)
};

struct RawToken {
    RawKind kind {};
    String lexeme;
    int line;
    int column;
    int aux = -1;
    PreprocessorKind ppKind = PreprocessorKind::None;

    RawToken(): kind(RawKind::Unknown), line(-1), column(-1) {}

    explicit RawToken(RawKind t, char* v, int l, int c)
        : kind(t), line(l), column(c) {
            lexeme += v;
        }

    explicit RawToken(RawKind t, char v, int l, int c, int a = -1)
        : kind(t), line(l), column(c), aux(a) {
            lexeme += v;
        }

    explicit RawToken(RawKind k, String lx, int l, int c, int a = -1)
    : kind(k), lexeme(std::move(lx)), line(l), column(c), aux(a) {}


    RawToken(RawKind t): kind(t), line(-1), column(-1) {}
};

// A token's original spelling in a caller-owned source. Offsets and lengths
// count bytes, not Unicode characters. No source pointer is retained.
// Unlike RawToken::lexeme, text() includes quotes/escapes, complete collapsed
// runs, and nested comment delimiters. SOF/EOF have empty ranges.
struct IndexedToken {
    // Wide fields first to avoid padding between the kind and the indices.
    size_t offset = 0;
    size_t length = 0;
    int line = -1;
    int column = -1;
    int aux = -1;
    RawKind kind = RawKind::Unknown;
    PreprocessorKind ppKind = PreprocessorKind::None;

    // Supply the same bytes that were scanned, in any buffer. The returned
    // view borrows that buffer; copying it into a String gives owning text.
    std::string_view text(std::string_view source) const {
        if (offset > source.size() || length > source.size() - offset)
            throw std::out_of_range("IndexedToken range exceeds source");
        return source.substr(offset, length);
    }
};

struct CommentPair {
    String start;   // "/*"
    String end;     // "*/"
    bool nestable = false; // C/C++ false, some langs true
};

// A single recognized preprocessor directive keyword, e.g. {"if", PreprocessorKind::If}.
struct PreprocessorKey {
    String key;
    PreprocessorKind kind = PreprocessorKind::Other;
};

struct PreprocessorConfig {
    String marker;                  // directive introducer, e.g. "#"; empty means disabled
    Vector<PreprocessorKey> keys;   // recognized keywords -> classification; unrecognized text after
                                     // the marker is left for normal tokenization (not a directive)
};

// A marker that switches the scanner to raw text lines: the lines it covers
// are not tokenized (quotes, brackets and comment starts are text), and each
// line is one RawKind::TextLine. What the text means (joining, folding,
// indentation) is the consumer's.
struct TextBlockConfig {
    String opener;          // e.g. """, or YAML's > and |
    // The block ends at `closer`. Empty: the block is the following lines
    // indented deeper than the opener's line (blank lines included), and
    // ends before the first line that is not.
    String closer;
    // The opener stands alone at the end of its line: after whitespace, and
    // followed only by whitespace or a line comment.
    bool endsLine = false;
};

// How one quote character's text is read. A quote with no QuoteConfig decodes
// C escapes (\n, \x41, \101, ...) and rejects unknown ones.
struct QuoteConfig {
    char quote = '"';
    // Keep the text exactly as written, escapes and doubled quotes included,
    // for a consumer that decodes it by its own language's rules (YAML's \u,
    // \U, folded line breaks). Lex only finds where the text ends.
    bool raw = false;
    // A backslash escapes the next character, so it cannot close the quote.
    // Off: a backslash is an ordinary character (shell and YAML single quotes).
    bool escapes = true;
    // Two quotes in a row stand for one quote character inside the text
    // (SQL, YAML single quotes) instead of closing it.
    bool doubled = false;
};

// A word written directly before a quote with optional reading rules:
// Python's r"", b"", C++'s L"", u8"". A configured prefix also permits a quote
// after a word when quotesNeedWordBoundary is enabled. The prefix stays an
// Identifier token directly before the text token.
struct StringPrefix {
    String prefix;
    // How the text after this prefix is read (its `quote` is ignored); unset,
    // as its quote reads without a prefix. Python's r"\d": raw, escapes on.
    bool hasReading = false;
    QuoteConfig reading;
};

struct CommentConfig {
    Vector<String> lineStarts;     // "#", "//", ";", "--", ...
    // A line comment starts only at the beginning of a line or after
    // whitespace, so a marker inside a word is text: YAML's and shell's
    // `url#frag`, `a#b`.
    bool lineStartsNeedSpace = false;
    Vector<CommentPair> blockPairs; // { "/*","*/" }, { "{-","-}" }, ...
    // Consume a comment through its terminator and continue scanning.
    // No Comment* tokens are emitted.
    bool skipComments = false;
    bool collapseDuplicates = true;

    // Nested here (not a separate Scanner constructor param) so that anything
    // already carrying a Scanner's CommentConfig - including a Structurizer
    // via LayoutConfig::scannerConfig - automatically knows the preprocessor
    // setup too.
    PreprocessorConfig preprocessor;

    // Nested here for the same reason: the Structurizer reads this config
    // and passes text lines through without applying indentation to them.
    Vector<TextBlockConfig> textBlocks;

    // Per-quote reading rules (see QuoteConfig); unlisted quotes decode C
    // escapes.
    Vector<QuoteConfig> quotes;

    // Words that may prefix a quote (see StringPrefix). Matched whole and
    // case-sensitively: list "F" and "Rb" too where the language allows them.
    Vector<StringPrefix> stringPrefixes;

    // Treat a quote immediately after a letter, digit or '_' as plain text
    // (op's, x'), unless a stringPrefix matches. Opt in for prose/YAML;
    // normal code recognizes quotes after identifiers without a prefix list.
    bool quotesNeedWordBoundary = false;
};



// If you don't already have this:
const char* rawKindToString(RawKind k);
const char* preprocessorKindToString(PreprocessorKind k);

// Escape lexeme so newlines/tabs are visible in debug output.
String escapeLexeme(const String& s);

void printRawTokens(const Vector<RawToken>& toks, std::ostream& os = std::cout);

// Optional: nice operator<< for RawToken
std::ostream& operator<<(std::ostream& os, const RawToken& t);






class Scanner {
    // Source state
public:
    const String source;
    size_t position = 0;
    size_t sourceLength = 0;
    char current = '\0';
    int line = 1;
    int column = 1;

    Vector<RawToken> rawTokens;
    RawToken noOpToken = RawToken(RawKind::NoOp);
    CommentConfig commentCfg;

    // Streaming mode: set commentCfg, then call stream() for each source.
    // The existing source-taking constructors continue to use scan().
    Scanner();
    Scanner(const char* sourceFile, const CommentConfig& cfg);
    Scanner(String src, CommentConfig cfg);

    // Only available on a default-constructed Scanner; otherwise throws
    // std::logic_error. Reads the complete supplied buffer synchronously,
    // without copying or retaining it. This is not chunked/incremental input.
    // Uses the current commentCfg, starts fresh on every call (also after
    // errors), and returns indices without populating rawTokens. The public
    // cursor is reset to an empty input when the call finishes or throws.
    Vector<IndexedToken> stream(std::string_view source);

    char next();
    bool hasNext();

    bool isWhiteSpace(char);
    bool isDigit(char);
    bool isTextBegin(char);
    // The character before the current one is a letter, digit or '_'.
    bool followsWordChar() const;
    // The configured prefix that is the whole word directly before the quote
    // at `position`, if any.
    const StringPrefix* stringPrefixBefore() const;
    bool isOperator(char);

    bool isCommentBegin(char);
    bool isPunctuation(char);
    bool isLetter(char);
    bool isSpecialChar(char);

    RawToken readIdentifier();
    RawToken readNumber();
    // `reading` overrides the quote's QuoteConfig (a string prefix's).
    RawToken readText(const QuoteConfig* reading = nullptr);

    RawToken readPunctuation();
    bool handleSpecialChar(char nextChar, char startChar, String& resultAccum);
    bool handleSpecialChars();
    void handleWhiteSpace();    
    RawToken readOperator();

    int matchBlockStartIndex() const;


    void readSource();


    // Call from your main scan loop *only when not inside a string/text literal*
    // Returns true if it consumed and emitted a comment delimiter token.
    bool tryScanCommentDelimiter();

    RawToken scanLineCommentBody();

    RawToken scanBlockCommentBody(int pairIndex);


    Vector<RawToken> scan();
private:
    bool streamOnly_ = false;
    std::string_view streamSource_;
    Vector<IndexedToken>* indexedOutput_ = nullptr;
    std::string_view input() const { return streamOnly_ ? streamSource_ : std::string_view(source); }
    void scanTokens();
    // The lexeme factory runs only for owning output. Streaming constructs
    // an IndexedToken directly from the components, without a RawToken.
    template <typename MakeLexeme>
    void emit(RawKind kind, size_t offset, size_t length, int line, int column, int aux,
              MakeLexeme&& makeLexeme, PreprocessorKind ppKind = PreprocessorKind::None);
    void emitRange(RawKind kind, size_t offset, size_t length, int line, int column, int aux = -1);

    // Readers advance the cursor without constructing tokens. KeepText=false
    // validates/consumes transformed text without materializing it.
    void consumeIdentifier(const std::array<uint16_t, 256>* classifications = nullptr);
    void consumeNumber();
    int consumeRun();
    template <bool KeepText>
    void consumeText(const QuoteConfig* reading, String* text);
    void scanText(const QuoteConfig* reading);
    std::string_view consumeLineCommentBody();
    template <bool KeepText>
    void consumeBlockCommentBody(int pairIndex, String* text);

    // ---- helpers ----
    bool inBounds(size_t pos) const;

    bool matchAt(size_t pos, const String& s) const;

    void advanceN(size_t n);

    // ---- comment delimiter scanning ----

    bool tryScanCommentStart();

    bool tryScanCommentEnd();





    bool matchBlockEndIndex(int i) const;

    RawToken scanBlockComment(int pairIndex);

    RawToken scanLineComment(const String& startLexeme);

    bool tryScanComment();

    // Recognizes commentCfg.preprocessor.marker followed by one of
    // commentCfg.preprocessor.keys; emits a single RawKind::Preprocessor
    // token classified via ppKind and leaves the remainder of the line
    // (condition expression, macro name, etc.) for normal tokenization.
    // Does nothing (returns false) when preprocessor.marker is empty, or
    // when the marker is present but not followed by a recognized keyword.
    bool tryScanPreprocessorDirective();

    // One character forward, counting a newline it passes.
    void step();
    // At a line end: "\n", or the "\r" of a "\r\n".
    bool atLineEnd() const;
    // A line comment marker may start here (see lineStartsNeedSpace).
    bool lineCommentMayStart() const;
    // Where the text starts: past a byte-order mark, if any.
    size_t textBegin = 0;
    const QuoteConfig* quoteConfig(char quote) const;
    // The configured text block opening here, if any (the longest opener).
    const TextBlockConfig* matchTextBlock() const;
    bool tryScanTextBlock();
    void scanIndentedText(int openerIndent);
    void scanDelimitedText(const String& closer);
    // Spaces and tabs before the first character of the line holding `pos`.
    int lineIndent(size_t pos) const;
};

using RunTimeError = std::runtime_error;


class ScannerError : public RunTimeError {
protected:
    String message;
    int line;
    int column;
    mutable String cache;

public:
    ScannerError() = default;
    ScannerError(const String& message, int line, int column);

    String errorString() const;

    const char* what() const noexcept override;

    ~ScannerError() = default;
};
