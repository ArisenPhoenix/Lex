// SPDX-License-Identifier: Apache-2.0
#include "Structurizer.hpp"

 Vector<RawToken> Structurizer::structurize(const Vector<RawToken>& in) {
    Vector<RawToken> out;
    out.reserve(in.size() + 32);

    indentStack_.clear();
    indentStack_.push_back(0);

    auto emit = [&](const RawToken& t) { out.push_back(t); };
    size_t i = 0;
    if (!in.empty() && in[0].kind == RawKind::SOF) {
        emit(in[0]);
        i = 1;
    }

    RawToken eofTok;
    bool sawEOF = false;

    if (cfg_.scopeMode == LayoutConfig::ScopeMode::Braces) {
        int scopeDepth = 0;

        for (; i < in.size(); ++i) {
            const RawToken& t = in[i];
            if (t.kind == RawKind::EOF_) {
                eofTok = t;
                sawEOF = true;
                break;
            }

            if (!cfg_.keepWhitespaceTokens && (t.kind == RawKind::Space || t.kind == RawKind::Tab)) {
                continue;
            }
            if (!cfg_.keepComments && isCommentToken(t)) {
                continue;
            }

            if (isScopeClose(t)) {
                if (scopeDepth <= 0) {
                    throw std::runtime_error("Brace scope error: unmatched closing scope token");
                }
                scopeDepth--;
                out.emplace_back(RawKind::Dedent, "", t.line, t.column, scopeDepth);
                emit(t);
                continue;
            }

            emit(t);

            if (isScopeOpen(t)) {
                scopeDepth++;
                out.emplace_back(RawKind::Indent, "", t.line, t.column, scopeDepth);
                continue;
            }
        }

        int eofLine = sawEOF ? eofTok.line : (in.empty() ? 1 : in.back().line);
        int eofCol  = sawEOF ? eofTok.column : (in.empty() ? 1 : in.back().column);
        while (scopeDepth > 0) {
            out.emplace_back(RawKind::Dedent, "", eofLine, eofCol, scopeDepth - 1);
            scopeDepth--;
        }

        if (sawEOF) out.push_back(eofTok);
        return out;
    }

    bool atLineStart = true;
    int pendingIndent = 0;

    int parenDepth = 0;

    for (; i < in.size(); ++i) {
        const RawToken& t = in[i];
        if (t.kind == RawKind::EOF_) {
            eofTok = t;
            sawEOF = true;
            break;
        }

        if (t.kind == RawKind::Punctuation && (t.lexeme == "(" || t.lexeme == "[")) {parenDepth++;}
        if (t.kind == RawKind::Punctuation && (t.lexeme == ")" || t.lexeme == "]")) parenDepth = std::max(0, parenDepth - 1);

        if (atLineStart) {
            if (t.kind == RawKind::Space) {
                pendingIndent += (t.aux > 0 ? t.aux : 1);
                if (cfg_.keepWhitespaceTokens) emit(t);
                continue;
            }
            if (t.kind == RawKind::Tab) {
                if (!cfg_.tabsAllowed) throw std::runtime_error("Tabs not allowed");
                int tabs = (t.aux > 0 ? t.aux : 1);
                pendingIndent += tabs * cfg_.tabWidth;
                if (cfg_.keepWhitespaceTokens) emit(t);
                continue;
            }

            if (t.kind == RawKind::Newline) {
                emit(t);
                atLineStart = true;
                pendingIndent = 0;
                continue;
            }

            if (isCommentToken(t)) {
                if (cfg_.keepComments) {emit(t);}
                atLineStart = false;
                continue;
            }

            if (parenDepth == 0) {applyIndent(pendingIndent, t, out);}

            pendingIndent = 0;
            atLineStart = false;

            emit(t);
            continue;
        }

        if (t.kind == RawKind::Newline) {
            if (cfg_.parenContinuation && parenDepth > 0) {
                atLineStart = true;
                pendingIndent = 0;
                continue;
            }

            emit(t);
            atLineStart = true;
            pendingIndent = 0;
            continue;
        }

        if (!cfg_.keepWhitespaceTokens && (t.kind == RawKind::Space || t.kind == RawKind::Tab)) {
            continue;
        }

        if (!cfg_.keepComments && isCommentToken(t)) {
            continue;
        }

        emit(t);
    }

    int eofLine = sawEOF ? eofTok.line : (in.empty() ? 1 : in.back().line);
    int eofCol  = sawEOF ? eofTok.column : (in.empty() ? 1 : in.back().column);
    while (indentStack_.size() > 1) {
        out.emplace_back(RawKind::Dedent, "", eofLine, eofCol, 0);
        indentStack_.pop_back();
    }

    // then emit EOF
    if (sawEOF) out.push_back(eofTok);
    return out;
}

bool Structurizer::isCommentToken(const RawToken& t) const {
    switch (t.kind)
    {
    case RawKind::Comment:
    case RawKind::CommentLineStart:
    case RawKind::CommentBlockStart:
    case RawKind::CommentBlockEnd:
        /* When it comes time to make comments functional */
        return true;
    
    default:
        return false;
    }
}

bool Structurizer::isScopeOpen(const RawToken& t) const {
    if (t.kind != RawKind::Punctuation) return false;
    for (const auto& opener : cfg_.scopeOpeners) {
        if (t.lexeme == opener) return true;
    }
    return false;
}

bool Structurizer::isScopeClose(const RawToken& t) const {
    if (t.kind != RawKind::Punctuation) return false;
    for (const auto& closer : cfg_.scopeClosers) {
        if (t.lexeme == closer) return true;
    }
    return false;
}

void Structurizer::applyIndent(int indent, const RawToken& atToken, Vector<RawToken>& out) {
    int cur = indentStack_.back();
    if (indent == cur) return;

    if (indent > cur) {
        indentStack_.push_back(indent);
        out.emplace_back(RawKind::Indent, "", atToken.line, atToken.column, indent);
        return;
    }

    while (indentStack_.size() > 1 && indentStack_.back() > indent) {
        indentStack_.pop_back();
        out.emplace_back(RawKind::Dedent, "", atToken.line, atToken.column, indent);
    }

    if (indentStack_.back() != indent) {
        throw std::runtime_error("Indentation error: unaligned dedent");
    }
}
