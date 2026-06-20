// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lex/Scanner.hpp"

enum class TokenType {
    Type,
    Keyword,
    Identifier,
    Number,
    String,
    Char,
    Bool,
    Text,

    VarDeclaration,
    VarAssignment,
    Variable,
    AccessorVariable,

    Operator,
    Punctuation,

    Indent,
    Dedent,
    Newline,
    EOF_Token,
    SOF_Token,

    Break,
    Else,
    If,
    Elif,
    DoWhile,
    Case,
    Default,
    Continue,

    Function,
    FunctionDef,
    FunctionCall,
    FunctionRef,

    Parameter,
    Argument,

    ClassDef,
    ClassCall,
    ClassRef,

    ClassMethodDef,
    ClassMethodCall,
    ClassMethodRef,
    ClassAttribute,

    ChainEntryPoint,

    Unknown,
    LeftBracket,
    RightBracket,
    LiteralArrowLeft,
    LiteralArrowRight,
    BraceLeft,
    BraceRight,
    LeftArrow,
    RightArrow,
    BeginTyping,

    NoOp,
    Comment,
};

struct Token {
    TokenType type;
    String value;
    int line;
    int column;

    Token(TokenType t, const String& v, int l, int c)
        : type(t), value(v), line(l), column(c) {}
};
