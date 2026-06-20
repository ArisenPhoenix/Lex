// SPDX-License-Identifier: Apache-2.0
#include "lex/Lexer.hpp"

#include <stdexcept>

static bool isCompoundPair(char a, char b) {
    switch (a) {
        case '<': return b == '=' || b == '<';
        case '>': return b == '=' || b == '>';
        case '=': return b == '=';
        case '!': return b == '=';
        case '+': return b == '=' || b == '+';
        case '-': return b == '=' || b == '-' || b == '>';
        case '*': return b == '=' || b == '*';
        case '/': return b == '=' || b == '/';
        case '&': return b == '&' || b == '=';
        case '|': return b == '|' || b == '=';
        case ':': return b == ':' || b == '=';
        default: return false;
    }
}

static bool isOpLike(const RawToken& t) {
    return t.kind == RawKind::Operator || t.kind == RawKind::Punctuation;
}

bool Lexer::tryEmitCompoundOp(const Vector<RawToken>& raw, size_t& i, Vector<Token>& out) {
    if (i + 1 >= raw.size()) return false;
    const RawToken& a = raw[i];
    const RawToken& b = raw[i + 1];

    if (!isOpLike(a) || !isOpLike(b)) return false;
    if (a.lexeme.empty() || b.lexeme.empty()) return false;

    const char ca = a.lexeme[0];
    const char cb = b.lexeme[0];
    if (!isCompoundPair(ca, cb)) return false;

    String two;
    two += ca;
    two += cb;

    if (two == ":=") {
        out.emplace_back(TokenType::VarAssignment, two, a.line, a.column);
        i += 2;
        return true;
    }

    out.emplace_back(TokenType::Operator, two, a.line, a.column);
    i += 2;
    return true;
}

static bool isTriviaRaw(const RawToken& t) {
    return t.kind == RawKind::Space || t.kind == RawKind::Tab;
}

bool Lexer::nextNonTriviaIsLParen(const Vector<RawToken>& raw, size_t i) const {
    for (size_t j = i + 1; j < raw.size(); ++j) {
        if (isTriviaRaw(raw[j])) continue;
        return raw[j].kind == RawKind::Punctuation && raw[j].lexeme == "(";
    }
    return false;
}

static bool nextNonTriviaIsDotOrColonColon(const Vector<RawToken>& raw, size_t i) {
    for (size_t j = i + 1; j < raw.size(); ++j) {
        if (isTriviaRaw(raw[j])) continue;
        if (raw[j].kind == RawKind::Punctuation && raw[j].lexeme == ".") return true;
        if (raw[j].kind == RawKind::Punctuation && raw[j].lexeme == ":" &&
            (j + 1 < raw.size()) && raw[j + 1].kind == RawKind::Punctuation && raw[j + 1].lexeme == ":")
            return true;
        return false;
    }
    return false;
}

bool Lexer::prevTokenWasDot(const Vector<Token>& out) const {
    if (out.empty()) return false;
    return out.back().value == ".";
}

TokenType Lexer::classifyIdentifier(const String& value, const Vector<Token>& out, const Vector<RawToken>& raw, size_t rawIndex) {
    TokenType type = TokenType::Variable;

    auto lastType = [&]() -> TokenType { return out.empty() ? TokenType::Unknown : out.back().type; };
    auto lastVal  = [&]() -> String    { return out.empty() ? "" : out.back().value; };

    const bool nextIsCall = nextNonTriviaIsLParen(raw, rawIndex);

    if (prevTokenWasDot(out) && nextIsCall) {
        insideArgs = true;
        return TokenType::ClassMethodCall;
    }

    if (prevTokenWasDot(out) && !nextIsCall) {
        return TokenType::Variable;
    }

    if (value == "Class") {
        return TokenType::ClassDef;
    }

    if (!out.empty() && lastType() == TokenType::ClassDef) {
        classes.insert(value);
        pendingClass = true;
        return TokenType::ClassRef;
    }

    if (value == "function" || value == "def") {
        insideParams = true;
        return insideClass ? TokenType::ClassMethodDef : TokenType::FunctionDef;
    }

    if (!out.empty() && (lastType() == TokenType::FunctionDef || lastType() == TokenType::ClassMethodDef)) {
        functions.insert(value);
        return insideClass ? TokenType::ClassMethodRef : TokenType::FunctionRef;
    }

    if (value == "true" || value == "false") return TokenType::Bool;
    if (value == "and" || value == "or" || value == "not") return TokenType::Operator;

    if (value == "var" || value == "const") return TokenType::VarDeclaration;

    if (keywords.count(value)) return TokenType::Keyword;

    if (primitives.count(value) && nextIsCall) {
        insideArgs = true;
        return TokenType::FunctionCall;
    }

    if (knownTypes.count(value) && nextNonTriviaIsDotOrColonColon(raw, rawIndex)) {
        return TokenType::ChainEntryPoint;
    }

    if (knownTypes.count(value) && !nextIsCall) {
        return TokenType::Type;
    }

    if ((cfg.nativeClasses.count(value) || classes.count(value)) && nextIsCall) {
        insideArgs = true;
        return TokenType::ClassCall;
    }

    if (nextIsCall) {
        insideArgs = true;
        return TokenType::FunctionCall;
    }

    if (insideArgs && !out.empty() && out.back().type == TokenType::Punctuation && !nextIsCall) {
        return TokenType::Argument;
    }

    if (insideParams && !nextIsCall) {
        return TokenType::Parameter;
    }

    if (functions.count(value)) { type = TokenType::FunctionRef; }
    if (classes.count(value))   { type = TokenType::ClassRef; }

    if (type == TokenType::Variable && value == "null") {
        return TokenType::String;
    }

    if (type == TokenType::Variable && lastVal() != "." && nextNonTriviaIsDotOrColonColon(raw, rawIndex)) {
        return TokenType::ChainEntryPoint;
    }

    if (type == TokenType::FunctionRef || type == TokenType::ClassMethodRef) {
        if (nextNonTriviaIsDotOrColonColon(raw, rawIndex) ||
            (!out.empty() && out.back().type == TokenType::VarDeclaration) ||
            (!out.empty() && out.back().type == TokenType::VarAssignment && nextNonTriviaIsDotOrColonColon(raw, rawIndex))) {
            return TokenType::Variable;
        }
    }

    return type;
}

Vector<Token> Lexer::lex(const Vector<RawToken>& raw, Vector<Token>& out) {
    out.reserve(raw.size());
    String s = "SOF";
    String e = "EOF";
    out.emplace_back(TokenType::SOF_Token, s, 0, 0);
    classes = cfg.nativeClasses;
    functions = cfg.nativeFuncs;
    keywords = cfg.keywords;
    primitives = cfg.primitiveCtors;
    knownTypes = cfg.knownTypes;

    for (size_t i = 0; i < raw.size(); ) {
        const RawToken& t = raw[i];
        if (t.kind == RawKind::SOF) {
            ++i;
            continue;
        }
        if (t.kind == RawKind::Space || t.kind == RawKind::Tab) { ++i; continue; }

        if (t.kind == RawKind::Comment || t.kind == RawKind::CommentLineStart ||
            t.kind == RawKind::CommentBlockStart || t.kind == RawKind::CommentBlockEnd) {
            ++i;
            continue;
        }

        if (t.kind == RawKind::Newline) {
            out.emplace_back(TokenType::Newline, "NewLine", t.line, t.column);
            ++i;
            continue;
        }

        if (t.kind == RawKind::Indent) {
            out.emplace_back(TokenType::Indent, "->", t.line, t.column);

            const int newIndent = (int)t.aux;

            currentIndent = newIndent;

            if (!indentStack.empty() && newIndent <= indentStack.back()) {
                throw std::runtime_error(
                    "Lexer: Indent aux not increasing. aux=" + std::to_string(newIndent) +
                    " prev=" + std::to_string(indentStack.back()) +
                    " at " + std::to_string(t.line) + ":" + std::to_string(t.column)
                );
            }

            indentStack.push_back(newIndent);

            if (pendingClass) {
                insideClass = true;
                classBodyIndentStack.push_back(newIndent);
                pendingClass = false;
            }

            ++i;
            continue;
        }

        if (t.kind == RawKind::Dedent) {
            out.emplace_back(TokenType::Dedent, "<-", t.line, t.column);

            const int newIndent = (int)t.aux;
            currentIndent = newIndent;

            if (indentStack.empty() || newIndent > indentStack.back()) {
                throw std::runtime_error(
                    "Lexer: Dedent aux larger than current indent. aux=" + std::to_string(newIndent) +
                    " prev=" + (indentStack.empty() ? String("EMPTY") : std::to_string(indentStack.back())) +
                    " at " + std::to_string(t.line) + ":" + std::to_string(t.column)
                );
            }

            while (indentStack.size() > 1 && indentStack.back() > newIndent) {
                indentStack.pop_back();
            }

            if (indentStack.back() != newIndent) {
                throw std::runtime_error(
                    "Lexer: Dedent aux does not match any prior indent level. aux=" + std::to_string(newIndent) +
                    " top=" + std::to_string(indentStack.back()) +
                    " at " + std::to_string(t.line) + ":" + std::to_string(t.column)
                );
            }

            if (insideClass && !classBodyIndentStack.empty() && newIndent < classBodyIndentStack.back()) {
                while (!classBodyIndentStack.empty() && newIndent < classBodyIndentStack.back()) {
                    classBodyIndentStack.pop_back();
                }
                insideClass = !classBodyIndentStack.empty();
            }

            ++i;
            continue;
        }

        if (tryEmitCompoundOp(raw, i, out)) { continue; }

        if (t.kind == RawKind::Number) {
            out.emplace_back(TokenType::Number, t.lexeme, t.line, t.column);
            ++i;
            continue;
        }
        if (t.kind == RawKind::String) {
            out.emplace_back(TokenType::String, t.lexeme, t.line, t.column);
            ++i;
            continue;
        }
        if (t.kind == RawKind::Char) {
            out.emplace_back(TokenType::Char, t.lexeme, t.line, t.column);
            ++i;
            continue;
        }
        if (t.kind == RawKind::Text) {
            out.emplace_back(TokenType::Text, t.lexeme, t.line, t.column);
            ++i;
            continue;
        }

        if (t.kind == RawKind::Identifier) {
            TokenType tt = classifyIdentifier(t.lexeme, out, raw, i);
            if (tt == TokenType::FunctionCall && prevTokenWasDot(out)) {
                tt = TokenType::ClassMethodCall;
            }
            out.emplace_back(tt, t.lexeme, t.line, t.column);
            ++i;
            continue;
        }

        if (t.kind == RawKind::Operator && t.lexeme == "=") {
            out.emplace_back(TokenType::VarAssignment, "=", t.line, t.column);
            ++i;
            continue;
        }

        if (t.kind == RawKind::Operator) {
            out.emplace_back(TokenType::Operator, t.lexeme, t.line, t.column);
            ++i;
            continue;
        }

        if (t.kind == RawKind::Punctuation) {
            if (t.lexeme == "[") { out.emplace_back(TokenType::LeftBracket, "[", t.line, t.column); }
            else if (t.lexeme == "]") { out.emplace_back(TokenType::RightBracket, "]", t.line, t.column); }
            else { out.emplace_back(TokenType::Punctuation, t.lexeme, t.line, t.column); }

            if (t.lexeme == ")") { insideArgs = false; insideParams = false; }
            ++i;
            continue;
        }

        if (t.kind == RawKind::EOF_) { break; }

        out.emplace_back(TokenType::Unknown, t.lexeme, t.line, t.column);
        ++i;
    }

    const RawToken& last = raw.empty() ? RawToken(RawKind::EOF_, e, 0, 0) : raw.back();
    out.emplace_back(TokenType::EOF_Token, e, last.line, last.column);
    return out;
}

Lexer::Lexer(LexerConfig lxCfg) { cfg = lxCfg; }

Lexer::Lexer() {}
