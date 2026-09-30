// SPDX-License-Identifier: Apache-2.0
#include "lex/Scanner.hpp"

#include <initializer_list>

namespace {

RawToken token(RawKind kind, String text, int line, int column) {
    return RawToken(kind, std::move(text), line, column);
}

void expect(const String& source, const CommentConfig& config,
            std::initializer_list<RawToken> expected) {
    Vector<RawToken> tokens;
    for (const auto& token : Scanner(source, config).scan()) {
        if (token.kind != RawKind::SOF && token.kind != RawKind::EOF_ &&
            token.kind != RawKind::Space && token.kind != RawKind::Newline)
            tokens.push_back(token);
    }
    if (tokens.size() != expected.size()) {
        printRawTokens(tokens, std::cerr);
        throw std::runtime_error("Unexpected token count for: " + source);
    }
    size_t i = 0;
    for (const auto& want : expected) {
        const auto& got = tokens[i++];
        if (got.kind != want.kind || got.lexeme != want.lexeme ||
            got.line != want.line || got.column != want.column) {
            std::cerr << "Expected " << want << " but got " << got << '\n';
            throw std::runtime_error("Unexpected token for: " + source);
        }
    }
}

void codeQuotes() {
    CommentConfig code;
    code.lineStarts = {"#"};
    // No prefix registration is needed just to recognize the quote. Braces
    // and comment markers in the string must not leak into later passes.
    for (const char* prefix : {"f", "F", "fr", "rF", "b", "u", "L", "u8", "custom"}) {
        const String word(prefix);
        expect(word + "\"{value} # text\"\nnext", code, {
            token(RawKind::Identifier, word, 1, 1),
            token(RawKind::String, "{value} # text", 1, static_cast<int>(word.size()) + 1),
            token(RawKind::Identifier, "next", 2, 1),
        });
    }
    expect("f'{value}'", code, {
        token(RawKind::Identifier, "f", 1, 1),
        token(RawKind::Char, "{value}", 1, 2),
    });
    expect("f\"a\\\"b\\n\"", code, {
        token(RawKind::Identifier, "f", 1, 1),
        token(RawKind::String, "a\"b\n", 1, 2),
    });
    expect("\"one\"\"two\"", code, {
        token(RawKind::String, "one", 1, 1),
        token(RawKind::String, "two", 1, 6),
    });
}

void yamlQuotes() {
    CommentConfig yaml;
    yaml.lineStarts = {"#"};
    yaml.skipComments = true;
    yaml.lineStartsNeedSpace = true;
    yaml.quotesNeedWordBoundary = true;
    yaml.quotes = {{'"', true, true, false}, {'\'', false, false, true}};
    expect("op's x' 3' name_'\n\"next\" # comment", yaml, {
        token(RawKind::Identifier, "op", 1, 1),
        token(RawKind::Unknown, "'", 1, 3),
        token(RawKind::Identifier, "s", 1, 4),
        token(RawKind::Identifier, "x", 1, 6),
        token(RawKind::Unknown, "'", 1, 7),
        token(RawKind::Number, "3", 1, 9),
        token(RawKind::Unknown, "'", 1, 10),
        token(RawKind::Identifier, "name_", 1, 12),
        token(RawKind::Unknown, "'", 1, 17),
        token(RawKind::String, "next", 2, 1),
    });
    expect(R"('it''s\path' "\u0041")", yaml, {
        token(RawKind::Char, "it's\\path", 1, 1),
        token(RawKind::String, "\\u0041", 1, 14),
    });
    expect("f\"", yaml, {
        token(RawKind::Identifier, "f", 1, 1),
        token(RawKind::Unknown, "\"", 1, 2),
    });
}

void prefixReadings() {
    CommentConfig config;
    StringPrefix raw{"r"};
    raw.hasReading = true;
    raw.reading.raw = true;
    config.stringPrefixes.push_back(raw);
    // Prefix readings work with both boundary policies. Matching must use
    // the whole word, so registering r must not affect other identifiers.
    for (bool boundary : {false, true}) {
        config.quotesNeedWordBoundary = boundary;
        expect(R"(r"\d\"x" "\n")", config, {
            token(RawKind::Identifier, "r", 1, 1),
            token(RawKind::String, R"(\d\"x)", 1, 2),
            token(RawKind::String, "\n", 1, 10),
        });
    }
    expect("other\"", config, {
        token(RawKind::Identifier, "other", 1, 1),
        token(RawKind::Unknown, "\"", 1, 6),
    });
    config.quotesNeedWordBoundary = false;
    expect(R"(other"\n")", config, {
        token(RawKind::Identifier, "other", 1, 1),
        token(RawKind::String, "\n", 1, 6),
    });
}

} // namespace

int main() {
    try {
        codeQuotes();
        yamlQuotes();
        prefixReadings();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
