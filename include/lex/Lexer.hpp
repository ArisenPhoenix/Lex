// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <unordered_set>

#include "lex/Structurizer.hpp"
#include "lex/Token.hpp"

struct LexerConfig {
    std::unordered_set<String> knownTypes;
    std::unordered_set<String> keywords;
    std::unordered_set<String> nativeClasses;
    std::unordered_set<String> nativeFuncs;
    std::unordered_set<String> primitiveCtors;
};

class Lexer {
public:
    Lexer(LexerConfig);
    Lexer();

    Vector<Token> lex(const Vector<RawToken>& raw, Vector<Token>&);

private:
    LexerConfig cfg;

    bool insideParams = false;
    bool insideArgs   = false;
    bool insideClass  = false;
    int  classIndentLevel = 0;
    int  currentIndent = 0;

    bool pendingClass = false;
    Vector<int> classBodyIndentStack;

    Vector<int> indentStack{0};

    std::unordered_set<String> classes;
    std::unordered_set<String> functions;
    std::unordered_set<String> primitives;

    std::unordered_set<String> keywords;
    std::unordered_set<String> knownTypes;

    TokenType classifyIdentifier(
        const String& value,
        const Vector<Token>& out,
        const Vector<RawToken>& raw,
        size_t rawIndex
    );

    bool nextNonTriviaIsLParen(const Vector<RawToken>& raw, size_t i) const;
    bool prevTokenWasDot(const Vector<Token>& out) const;

    bool tryEmitCompoundOp(
        const Vector<RawToken>& raw, size_t& i,
        Vector<Token>& out
    );

    void onIndent(int);
    void onDedent(int);
};
