// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include "lex/Scanner.hpp"

enum class TokKind : uint16_t {
    SOF, EOF_,
    Identifier, Number, String, Char, Text,
    Operator, Punctuation,

    Newline,
    Indent,
    Dedent,

    CommentLineStart,
    CommentBlockStart,
    CommentBlockEnd,
    Comment,

    Space, 
    Tab,
};


struct LayoutConfig {
    enum class ScopeMode : uint8_t {
        Indent,
        Braces,
    };

    int tabWidth = 4;
    bool tabsAllowed = true;
    bool spacesAllowed = true;

    bool keepWhitespaceTokens = false;   // drop Space/Tab tokens unless debugging
    bool skipWhitespace = false;         // drop Space/Tab even if keepWhitespaceTokens is true
    bool keepComments = true;            // keep comment tokens or drop them
    bool commentsAreLineContent = false; // for indentation: whether comment-only lines count (usually false)
    // Preprocessor directive markers (see PreprocessorConfig) never participate
    // in scope tracking regardless of this flag - it only controls whether the
    // marker token itself passes through to the output. Defaults to dropped,
    // since no current consumer (MERK, SAL) configures a Scanner preprocessor
    // marker in the first place, so this is a no-op for them either way.
    bool keepPreprocessor = false;

    // “line continuation” / newline suppression (optional for now)
    bool backslashContinuation = false;
    bool parenContinuation = true;       // ignore newline inside (), [], {}
    bool collapseDuplicates = true;                 // collapse multiple same items into one aux specifying the count (e.g. multiple spaces, tabs, semicolons, newlines)

    // Scope handling mode.
    ScopeMode scopeMode = ScopeMode::Indent;
    // Do not inject Indent/Dedent or pair brace scopes. Tokens pass through
    // after whitespace/comment filtering.
    bool omitScope = false;

    // Used when scopeMode == Braces
    Vector<String> scopeOpeners = {"{"};
    Vector<String> scopeClosers = {"}"};

    // The config of the Scanner that produced the RawTokens this Structurizer
    // will process. Structurizer depends on Scanner's output, so it needs to
    // know things like comment delimiters and whether identical-run collapsing
    // (aux counts) is in effect to interpret that output correctly. Defaults
    // to a default-constructed CommentConfig when the caller has no Scanner
    // (or doesn't care to share its config).
    CommentConfig scannerConfig;
};


class Structurizer {
public:
    // No explicit Scanner config: uses cfg.scannerConfig as given (a
    // default-constructed CommentConfig unless the caller set one).
    Structurizer(LayoutConfig cfg) : cfg_(std::move(cfg)) {}

    // Explicit Scanner config: pass the CommentConfig actually used to build
    // the Scanner whose output will be structurized, so this Structurizer is
    // always aware of it.
    Structurizer(LayoutConfig cfg, CommentConfig scannerCfg) : cfg_(std::move(cfg)) {
        cfg_.scannerConfig = std::move(scannerCfg);
    }

    Vector<RawToken> structurize(const Vector<RawToken>& in);

private:
    LayoutConfig cfg_;
    Vector<int> indentStack_;

    bool isCommentToken(const RawToken& t) const;
    bool isPreprocessorToken(const RawToken& t) const;
    bool isScopeOpen(const RawToken& t) const;
    bool isScopeClose(const RawToken& t) const;
    bool dropWhitespace() const;
    bool dropComment(const RawToken& t) const;
    bool dropPreprocessor(const RawToken& t) const;

    void applyIndent(int indent, const RawToken& atToken, Vector<RawToken>& out);
};
