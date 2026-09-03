// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
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

struct CommentConfig {
    Vector<String> lineStarts;     // "#", "//", ";", "--", ...
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

    Scanner(const char* sourceFile, const CommentConfig& cfg);
    Scanner(String src, CommentConfig cfg);

    char next();
    bool hasNext();

    bool isWhiteSpace(char);
    bool isDigit(char);
    bool isTextBegin(char);
    bool isOperator(char);

    bool isCommentBegin(char);
    bool isPunctuation(char);
    bool isLetter(char);
    bool isSpecialChar(char);

    RawToken readIdentifier();
    RawToken readNumber();
    RawToken readText();

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