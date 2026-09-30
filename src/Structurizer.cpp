// SPDX-License-Identifier: Apache-2.0
#include "lex/Structurizer.hpp"

#include <algorithm>
#include <array>

namespace {
// Scanner punctuation is one byte. Keep the general string matching path
// for callers that supply their own, possibly multi-character, RawTokens.
std::array<unsigned char, 256> delimiterFlags(const Vector<String>& openers,
                                              const Vector<String>& closers) {
    std::array<unsigned char, 256> flags{};
    for (const auto& opener : openers)
        if (opener.size() == 1) flags[static_cast<unsigned char>(opener[0])] |= 1;
    for (const auto& closer : closers)
        if (closer.size() == 1) flags[static_cast<unsigned char>(closer[0])] |= 2;
    return flags;
}
}

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

    if (cfg_.omitScope) {
        for (; i < in.size(); ++i) {
            const RawToken& t = in[i];
            if (t.kind == RawKind::EOF_) {
                eofTok = t;
                sawEOF = true;
                break;
            }
            if (dropWhitespace() && (t.kind == RawKind::Space || t.kind == RawKind::Tab)) {
                continue;
            }
            if (dropComment(t)) continue;
            if (dropPreprocessor(t)) continue;
            emit(t);
        }
        if (sawEOF) out.push_back(eofTok);
        return out;
    }

    if (cfg_.scopeMode == LayoutConfig::ScopeMode::Braces) {
        int scopeDepth = 0;
        const auto scopes = delimiterFlags(cfg_.scopeOpeners, cfg_.scopeClosers);

        for (; i < in.size(); ++i) {
            const RawToken& t = in[i];
            if (t.kind == RawKind::EOF_) {
                eofTok = t;
                sawEOF = true;
                break;
            }

            if (dropWhitespace() && (t.kind == RawKind::Space || t.kind == RawKind::Tab)) {
                continue;
            }
            if (dropComment(t)) {
                continue;
            }

            if (dropPreprocessor(t)) {
                continue;
            }

            const bool singlePunctuation = t.kind == RawKind::Punctuation && t.lexeme.size() == 1;
            const unsigned char flags = singlePunctuation ? scopes[static_cast<unsigned char>(t.lexeme[0])] : 0;
            if (singlePunctuation ? (flags & 2) != 0 : isScopeClose(t)) {
                // Scanner::readPunctuation folds a run of identical closers (e.g. "}}}")
                // into a single token with aux set to the run length, so a closer here
                // can account for more than one scope level.
                const int count = (t.aux > 0 ? t.aux : 1);
                if (scopeDepth < count) {
                    throw std::runtime_error("Brace scope error: unmatched closing scope token");
                }
                scopeDepth -= count;
                out.emplace_back(RawKind::Dedent, "", t.line, t.column, scopeDepth);
                emit(t);
                continue;
            }

            emit(t);

            if (singlePunctuation ? (flags & 1) != 0 : isScopeOpen(t)) {
                const int count = (t.aux > 0 ? t.aux : 1);
                scopeDepth += count;
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
    const auto brackets = delimiterFlags(cfg_.bracketOpeners, cfg_.bracketClosers);
    // The previous content token on this line was an indent marker that
    // indented its content (so a following marker may indent again).
    bool afterMarker = false;
    // Inside a text block (scannerConfig.textBlocks): from its opener through
    // its lines (and closer). Its newlines are line breaks of the text, kept
    // even inside brackets, and its lines do not indent.
    bool inTextBlock = false;

    for (; i < in.size(); ++i) {
        const RawToken& t = in[i];
        if (t.kind == RawKind::EOF_) {
            eofTok = t;
            sawEOF = true;
            break;
        }

        // Whether a line continues inside brackets is decided by the depth
        // before its first token: a line opening with '{' still starts at
        // depth 0 (and indents), one opening with ')' is still inside.
        const int depthBefore = parenDepth;
        if (t.kind == RawKind::Punctuation) {
            int delta;
            if (t.lexeme.size() == 1) {
                const auto flags = brackets[static_cast<unsigned char>(t.lexeme[0])];
                const int count = t.aux > 0 ? t.aux : 1;
                delta = (flags & 1) ? count : (flags & 2) ? -count : 0;
            } else {
                delta = bracketDelta(t);
            }
            parenDepth = std::max(0, parenDepth + delta);
        }
        if (t.kind == RawKind::TextBlockStart) inTextBlock = true;
        else if (t.kind != RawKind::TextLine && t.kind != RawKind::Newline && t.kind != RawKind::TextBlockEnd &&
                 t.kind != RawKind::Space && t.kind != RawKind::Tab && !isCommentToken(t))
            inTextBlock = false;

        if (atLineStart) {
            if (t.kind == RawKind::Space) {
                pendingIndent += (t.aux > 0 ? t.aux : 1);
                if (!dropWhitespace()) emit(t);
                continue;
            }
            if (t.kind == RawKind::Tab) {
                if (!cfg_.tabsAllowed) throw std::runtime_error("Tabs not allowed");
                int tabs = (t.aux > 0 ? t.aux : 1);
                pendingIndent += tabs * cfg_.tabWidth;
                if (!dropWhitespace()) emit(t);
                continue;
            }

            if (t.kind == RawKind::Newline) {
                emit(t);
                atLineStart = true;
                pendingIndent = 0;
                continue;
            }

            if (isCommentToken(t)) {
                if (!dropComment(t)) emit(t);
                atLineStart = false;
                continue;
            }

            if (isPreprocessorToken(t)) {
                if (!dropPreprocessor(t)) emit(t);
                atLineStart = false;
                continue;
            }

            // A text block's lines (scannerConfig.textBlocks) are raw text:
            // their indentation is content, not scope.
            if (inTextBlock && (t.kind == RawKind::TextLine || t.kind == RawKind::TextBlockEnd)) {
                emit(t);
                if (t.kind == RawKind::TextBlockEnd) inTextBlock = false;
                atLineStart = false;
                continue;
            }

            if (depthBefore == 0) {applyIndent(pendingIndent, t, out);}

            pendingIndent = 0;
            atLineStart = false;

            emit(t);
            afterMarker = parenDepth == 0 && applyMarkerIndent(in, i, out);
            continue;
        }

        if (t.kind == RawKind::TextBlockEnd) inTextBlock = false;

        if (t.kind == RawKind::Newline) {
            if (cfg_.parenContinuation && parenDepth > 0 && !inTextBlock) {
                atLineStart = true;
                pendingIndent = 0;
                continue;
            }

            emit(t);
            atLineStart = true;
            pendingIndent = 0;
            continue;
        }

        if (dropWhitespace() && (t.kind == RawKind::Space || t.kind == RawKind::Tab)) {
            continue;
        }

        if (dropComment(t)) {
            continue;
        }

        if (dropPreprocessor(t)) {
            continue;
        }

        emit(t);
        if (t.kind != RawKind::Space && t.kind != RawKind::Tab) {
            afterMarker = afterMarker && parenDepth == 0 && applyMarkerIndent(in, i, out);
        }
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

bool Structurizer::dropWhitespace() const {
    return cfg_.skipWhitespace || !cfg_.keepWhitespaceTokens;
}

bool Structurizer::dropComment(const RawToken& t) const {
    return !cfg_.keepComments && isCommentToken(t);
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

bool Structurizer::isPreprocessorToken(const RawToken& t) const {
    return t.kind == RawKind::Preprocessor;
}

bool Structurizer::dropPreprocessor(const RawToken& t) const {
    return !cfg_.keepPreprocessor && isPreprocessorToken(t);
}

int Structurizer::bracketDelta(const RawToken& t) const {
    if (t.kind != RawKind::Punctuation) return 0;
    const int count = (t.aux > 0 ? t.aux : 1);
    for (const auto& opener : cfg_.bracketOpeners) {
        if (t.lexeme == opener) return count;
    }
    for (const auto& closer : cfg_.bracketClosers) {
        if (t.lexeme == closer) return -count;
    }
    return 0;
}

bool Structurizer::isIndentMarker(const RawToken& t) const {
    if (t.aux > 1) return false;  // a collapsed run ("--") is not the marker
    for (const auto& marker : cfg_.indentMarkers) {
        if (t.lexeme == marker) return true;
    }
    return false;
}

bool Structurizer::applyMarkerIndent(const Vector<RawToken>& in, size_t i, Vector<RawToken>& out) {
    if (!isIndentMarker(in[i])) return false;
    size_t j = i + 1;
    if (j >= in.size() || (in[j].kind != RawKind::Space && in[j].kind != RawKind::Tab)) return false;
    while (j < in.size() && (in[j].kind == RawKind::Space || in[j].kind == RawKind::Tab)) ++j;
    if (j >= in.size()) return false;
    const RawToken& content = in[j];
    if (content.kind == RawKind::Newline || content.kind == RawKind::EOF_ || isCommentToken(content)) return false;
    const int indent = content.column - 1;
    if (indent <= indentStack_.back()) return false;
    indentStack_.push_back(indent);
    out.emplace_back(RawKind::Indent, "", content.line, content.column, indent);
    return true;
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
