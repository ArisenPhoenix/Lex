// SPDX-License-Identifier: Apache-2.0
#include "lex/Scanner.hpp"

#include <cctype>
#include <algorithm>
#include <optional>
#include <string_view>

namespace {
enum CharFlags : uint16_t {
    IdentifierStart = 1 << 0,
    IdentifierPart = 1 << 1,
    Digit = 1 << 2,
    Operator = 1 << 3,
    Punctuation = 1 << 4,
    Quote = 1 << 5,
    CommentStart = 1 << 6,
    PreprocessorStart = 1 << 7,
    TextBlockStart = 1 << 8,
};

constexpr auto basicClassifications = [] {
    std::array<uint16_t, 256> table{};
    for (unsigned char c : std::string_view("+-*/%=<>&|!")) table[c] |= Operator;
    for (unsigned char c : std::string_view(":;.,$@?()[]{}#")) table[c] |= Punctuation;
    for (unsigned char c : std::string_view("'\"`")) table[c] |= Quote;
    table['_'] |= IdentifierStart | IdentifierPart;
    return table;
}();

auto classifyCharacters(const CommentConfig& config) {
    auto table = basicClassifications;
    // Match isLetter() in the current C locale, including high bytes. Build
    // afresh for each scan so locale and public config changes are respected.
    for (unsigned c = 0; c < table.size(); ++c) {
        if (std::isalpha(c)) table[c] |= IdentifierStart | IdentifierPart;
        if (std::isdigit(c)) table[c] |= Digit | IdentifierPart;
    }
    auto markStart = [&](const String& delimiter, CharFlags flag) {
        if (!delimiter.empty()) table[static_cast<unsigned char>(delimiter.front())] |= flag;
    };
    for (const auto& start : config.lineStarts) markStart(start, CommentStart);
    for (const auto& pair : config.blockPairs) markStart(pair.start, CommentStart);
    markStart(config.preprocessor.marker, PreprocessorStart);
    for (const auto& block : config.textBlocks) markStart(block.opener, TextBlockStart);
    return table;
}
}

Vector<RawToken> Scanner::scan() {
    if (streamOnly_) throw std::logic_error("Default-constructed Scanner requires stream(source)");
    scanTokens();
    return rawTokens;
}

Scanner::Scanner() : streamOnly_(true) {}

Vector<IndexedToken> Scanner::stream(std::string_view src) {
    if (!streamOnly_) throw std::logic_error("stream(source) requires a default-constructed Scanner");
    Vector<IndexedToken> tokens;
    streamSource_ = src;
    indexedOutput_ = &tokens;
    position = 0;
    sourceLength = src.size();
    current = src.empty() ? '\0' : src.front();
    line = column = 1;
    textBegin = 0;
    auto releaseSource = [&] {
        streamSource_ = {};
        indexedOutput_ = nullptr;
        position = sourceLength = textBegin = 0;
        current = '\0';
        line = column = 1;
    };
    try {
        if (src.substr(0, 3) == "\xEF\xBB\xBF") advanceN(textBegin = 3);
        scanTokens();
    } catch (...) {
        releaseSource();
        throw;
    }
    releaseSource();
    return tokens;
}

template <typename MakeLexeme>
void Scanner::emit(RawKind kind, size_t offset, size_t length, int atLine, int atColumn, int aux,
                   MakeLexeme&& makeLexeme, PreprocessorKind ppKind) {
    if (indexedOutput_) {
        indexedOutput_->emplace_back(offset, length, atLine, atColumn, aux, kind, ppKind);
    } else {
        rawTokens.emplace_back(kind, makeLexeme(), atLine, atColumn, aux);
        rawTokens.back().ppKind = ppKind;
    }
}

void Scanner::emitRange(RawKind kind, size_t offset, size_t length, int atLine, int atColumn, int aux) {
    emit(kind, offset, length, atLine, atColumn, aux,
         [&] { return String(input().substr(offset, length)); });
}

void Scanner::scanTokens() {
    const auto classifications = classifyCharacters(commentCfg);
    emitRange(RawKind::SOF, 0, 0, -1, -1);
    while (position < sourceLength) {
        
        handleWhiteSpace();
        const auto flags = classifications[static_cast<unsigned char>(current)];
        if ((flags & CommentStart) && tryScanComment()) {
            continue;
        }

        if ((flags & PreprocessorStart) && tryScanPreprocessorDirective()) {
            continue;
        }

        if ((flags & TextBlockStart) && tryScanTextBlock()) {
            continue;
        }

        const size_t begin = position;
        const int startLine = line, startColumn = column;

        if (flags & Digit) {
             consumeNumber();
             emitRange(RawKind::Number, begin, position - begin, startLine, startColumn);
             continue;
        }

        if (flags & Quote) {
            const StringPrefix* prefix = stringPrefixBefore();
            if (!commentCfg.quotesNeedWordBoundary || !followsWordChar() || prefix) {
                scanText(prefix && prefix->hasReading ? &prefix->reading : nullptr);
                continue;
            }
            // Embedded quotes in prose remain Unknown tokens for the consumer.
        }


        if (flags & Operator) {
            const char ch = current;
            const int count = consumeRun();
            emit(RawKind::Operator, begin, position - begin, startLine, startColumn, count,
                 [ch] { return String(1, ch); });
            continue;
        }

        if (flags & Punctuation) {
            const char ch = current;
            const int count = consumeRun();
            emit(RawKind::Punctuation, begin, position - begin, startLine, startColumn, count,
                 [ch] { return String(1, ch); });
            continue;
        }

        if (flags & IdentifierStart) {
            consumeIdentifier(&classifications);
            emitRange(RawKind::Identifier, begin, position - begin, startLine, startColumn);
            continue;

        }

        if (!hasNext()) {
            break;
        }
        emitRange(RawKind::Unknown, position, 1, line, column);
        advanceN(1);
        
    }

    emit(RawKind::EOF_, sourceLength, 0, line, column, -1, [] { return String("EOF"); });
}

Scanner::Scanner(String src, CommentConfig cfg)
    : source(std::move(src)), commentCfg(std::move(cfg)) {
    sourceLength = source.size();
    current = (sourceLength > 0) ? source[0] : '\0';
    // A UTF-8 byte-order mark is not text. Columns stay byte offsets, so the
    // first token after it is at column 4.
    if (source.compare(0, 3, "\xEF\xBB\xBF") == 0) advanceN(textBegin = 3);
}

Scanner::Scanner(const char* src, const CommentConfig& cfg) : Scanner(String(src), cfg) {}

char Scanner::next() {
    column++;
    if (position + 1 >= sourceLength) {
        position = sourceLength;
        current = '\0';
        return current;
    }
    position++;
    current = input()[position];
    return current;
}

bool Scanner::hasNext() { return position < sourceLength; }


bool Scanner::isWhiteSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

bool Scanner::isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c));
}

void Scanner::handleWhiteSpace() {
    while ((isWhiteSpace(current) || atLineEnd()) && hasNext()) {
        int startLine = line;
        int startCol = column;
        if (atLineEnd()) {
            const size_t begin = position;
            // "\r\n" is one line end.
            int numNewLines = 0;
            while (atLineEnd()) {
                if (current == '\r') next();
                ++numNewLines; line++; column = 0; next();
            }
            emit(RawKind::Newline, begin, position - begin, startLine, startCol, numNewLines,
                 [] { return String("\n"); });
        }
        // Each run is where it starts, not where the loop pass started (a
        // tab after a line break is on the next line).
        if (current == '\t') {
            const size_t begin = position;
            startLine = line;
            startCol = column;
            int numTabs = 0;
            while (current == '\t') { ++numTabs; next(); }
            emit(RawKind::Tab, begin, position - begin, startLine, startCol, numTabs,
                 [] { return String("    "); });
        }

        if (current == ' ') {
            const size_t begin = position;
            startLine = line;
            startCol = column;
            int numSpaces = 0;
            while (current == ' ') { ++numSpaces; next(); }
            emit(RawKind::Space, begin, position - begin, startLine, startCol, numSpaces,
                 [] { return String(" "); });
            continue;
        }

    }
}

bool Scanner::isSpecialChar(char c) {
    switch (c) {
        case 'n':
        case 't':
        case 'r':
        case '\\':
        case '\'':
        case '"':
            return true;
        default:
            return false;
    }

}

bool Scanner::handleSpecialChar(char nextChar, char startChar, String& resultAccum) {
    if (nextChar == startChar) { return true; }
    if (isSpecialChar(nextChar)) {
        resultAccum += '\\' + nextChar;
        return true;
    }
    return false;
}

bool Scanner::isOperator(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || 
        c == '%' || c == '=' || c == '<' || c == '>' || 
        c == '&' || c == '|' || c == '!';
}

bool Scanner::isPunctuation(char c) {
    return c == ':' || c == ';' || c == '.' || c == ',' || c == '$' || c == '@' || c == '?'
        || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '#';
}

bool Scanner::isTextBegin(char c) {
    return c == '\'' || c == '"' || c == '`';
}

bool Scanner::followsWordChar() const {
    if (position == 0) return false;
    const unsigned char prev = static_cast<unsigned char>(input()[position - 1]);
    return std::isalnum(prev) || prev == '_';
}

bool Scanner::isLetter(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool Scanner::isCommentBegin(char c) {
    String s(1, c);
    for (auto& starts : commentCfg.lineStarts) {
        if (starts == s) {
            return true;
        }
    }
    for (auto& pair : commentCfg.blockPairs) {
        
        if (pair.start == s) {
            return true;
        }
    }

    return false;
}


RawToken Scanner::readOperator() {
    if (!isOperator(current)) {
        throw ScannerError("Not Operator in readOperator -> " + std::to_string(current), line, column);
    }

    const char ch = current;
    const int startLine = line;
    const int startCol = column;
    const int count = consumeRun();
    
    return RawToken(RawKind::Operator, ch, startLine, startCol, count);
}

RawToken Scanner::readPunctuation() {
    if (!isPunctuation(current)) {
        throw ScannerError("Not Punctuation in readPunctuation -> " + std::to_string(current), line, column);
    }

    const char ch = current;
    const int startLine = line;
    const int startCol = column;
    const int count = consumeRun();
    return RawToken(RawKind::Punctuation, ch, startLine, startCol, count);
}

int Scanner::consumeRun() {
    const char ch = current;
    int count = 0;
    if (commentCfg.collapseDuplicates) {
        while (current == ch) {
            ++count;
            next();
        }
    } else {
        count = 1;
        next();
    }

    return count;
}

const StringPrefix* Scanner::stringPrefixBefore() const {
    if (commentCfg.stringPrefixes.empty()) return nullptr;
    auto word = [&](size_t i) {
        const unsigned char c = static_cast<unsigned char>(input()[i]);
        return std::isalnum(c) || c == '_';
    };
    size_t start = position;
    while (start > 0 && word(start - 1)) --start;
    const std::string_view before(input().data() + start, position - start);
    for (const auto& p : commentCfg.stringPrefixes)
        if (before == p.prefix) return &p;
    return nullptr;
}

RawToken Scanner::readText(const QuoteConfig* reading) {
    const int startColumn = column, startLine = line;
    const RawKind type = current == '\'' ? RawKind::Char : current == '"' ? RawKind::String : RawKind::Text;
    String result;
    consumeText<true>(reading, &result);
    return RawToken(type, std::move(result), startLine, startColumn);
}

void Scanner::scanText(const QuoteConfig* reading) {
    const size_t begin = position;
    const int startColumn = column, startLine = line;
    const RawKind type = current == '\'' ? RawKind::Char : current == '"' ? RawKind::String : RawKind::Text;
    std::optional<String> result;
    if (indexedOutput_) {
        consumeText<false>(reading, nullptr);
    } else {
        result.emplace();
        consumeText<true>(reading, &*result);
    }
    emit(type, begin, position - begin, startLine, startColumn, -1,
         [&] { return std::move(*result); });
}

template <bool KeepText>
void Scanner::consumeText(const QuoteConfig* reading, String* result) {
    if (!isTextBegin(current)) { throw ScannerError("Not Text in readText -> " + std::to_string(current), line, column); }
    const char startChar = current;
    auto appendChar = [&](char c) { if constexpr (KeepText) *result += c; };
    const QuoteConfig* quote = reading ? reading : quoteConfig(startChar);
    const bool raw = quote && quote->raw;
    const bool escapes = !quote || quote->escapes;
    const bool doubled = quote && quote->doubled;
    next();

    while (hasNext()) {
        if (current == startChar) {
            if (!doubled || position + 1 >= sourceLength || input()[position + 1] != startChar) break;
            // A doubled quote is one quote character of the text.
            appendChar(startChar);
            if (raw) appendChar(startChar);
            next();
            next();
            continue;
        }
        if (current == '\\' && escapes && raw) {
            // Kept as written; the escaped character cannot close the text.
            appendChar(current);
            step();
            if (!hasNext()) throw ScannerError("Unfinished escape sequence in string literal", line, column);
            appendChar(current);
            step();
            continue;
        }
        if (current == '\\' && escapes) {
            if (!hasNext()) {
                throw ScannerError("Unfinished escape sequence in string literal", line, column);
            }
            next();
            const char escaped = current;
            switch (escaped) {
                case 'n':  appendChar('\n'); break;
                case 't':  appendChar('\t'); break;
                case 'r':  appendChar('\r'); break;
                case 'a':  appendChar('\a'); break;
                case 'b':  appendChar('\b'); break;
                case 'f':  appendChar('\f'); break;
                case 'v':  appendChar('\v'); break;
                case '\\': appendChar('\\'); break;
                case '\n': 
                case '\r': /* C/C++ line continuation: \ + newline removed */ break;
                case '?':  appendChar('?'); break; /* C++ trigraph prevention */
                case '\'': appendChar('\''); break;
                case '"':  appendChar('"'); break;
                case '`':  appendChar('`'); break;
                default:
                    if (escaped == 'x') {
                        auto hexValue = [](char d) -> int {
                            if (d >= '0' && d <= '9') return static_cast<int>(d - '0');
                            if (d >= 'a' && d <= 'f') return 10 + static_cast<int>(d - 'a');
                            if (d >= 'A' && d <= 'F') return 10 + static_cast<int>(d - 'A');
                            return -1;
                        };
                        int value = 0;
                        int digits = 0;
                        while ((position + 1) < sourceLength) {
                            const char d = input()[position + 1];
                            const int hv = hexValue(d);
                            if (hv < 0) break;
                            next();
                            value = (value * 16) + hv;
                            ++digits;
                        }
                        if (digits == 0) {
                            throw ScannerError("Invalid hex escape in string literal", line, column);
                        }
                        appendChar(static_cast<char>(value & 0xFF));
                    } else if (escaped >= '0' && escaped <= '7') {
                        // C/C++ octal escapes: \0 ... \777 (consume up to 3 octal digits total).
                        int value = static_cast<int>(escaped - '0');
                        int digits = 1;
                        while (digits < 3 && (position + 1) < sourceLength) {
                            const char d = input()[position + 1];
                            if (d < '0' || d > '7') break;
                            next();
                            value = (value * 8) + static_cast<int>(current - '0');
                            ++digits;
                        }
                        appendChar(static_cast<char>(value & 0xFF));
                    } else if (escaped == startChar) {
                        appendChar(startChar);
                    } else {
                        throw ScannerError("Unknown escape in string literal", line, column);
                    }
                    break;
            }
            step();  // a line continuation passes a newline
            continue;

        }

        // Copy ordinary text in one piece. Newlines and quote/escape rules
        // still go through the stateful paths above.
        const size_t begin = position;
        size_t end = begin;
        while (end < sourceLength && input()[end] != startChar && input()[end] != '\n' &&
               (!escapes || input()[end] != '\\')) ++end;
        if constexpr (KeepText) result->append(input(), begin, end - begin);
        advanceN(end - begin);
        if (current == '\n') {
            appendChar(current);
            step();
        }
    }

    if (hasNext() && current != startChar) { // will need to use a current check on hasNext() or just remove it
        throw ScannerError("Unmatched quote for " + std::to_string(startChar), line, column);
    }

    next();
}

RawToken Scanner::readNumber() {
    if (!isDigit(current)) {
        throw ScannerError("Not Digit in readNumber -> " + std::to_string((int)current), line, column);
    }

    const int startLine = line;
    const int startCol  = column;
    const size_t begin = position;
    consumeNumber();
    return RawToken(RawKind::Number, String(input().substr(begin, position - begin)), startLine, startCol);
}

void Scanner::consumeNumber() {
    const size_t begin = position;
    size_t end = begin;
    bool seenDot = false;
    while (end < sourceLength) {
        if (isDigit(input()[end])) {
            ++end;
            continue;
        }

        if (!seenDot && input()[end] == '.' && (end + 1) < sourceLength && isDigit(input()[end + 1]))
        {
            seenDot = true;
            ++end;
            continue;
        }

        break;
    }

    advanceN(end - begin);
}

RawToken Scanner::readIdentifier() {
    if (!isLetter(current)) throw ScannerError("Not Letter in readIdentifier -> " + std::to_string(current), line, column);

    int startCol = column;
    int startLine = line;
    const size_t begin = position;
    consumeIdentifier();
    return RawToken(RawKind::Identifier, String(input().substr(begin, position - begin)), startLine, startCol);
}

void Scanner::consumeIdentifier(const std::array<uint16_t, 256>* classifications) {
    const size_t begin = position;
    size_t end = begin;
    if (classifications) {
        const auto bytes = input();
        while (end < sourceLength &&
               ((*classifications)[static_cast<unsigned char>(bytes[end])] & IdentifierPart)) ++end;
    } else {
        // Public readIdentifier() also works outside scan(), without a table.
        while (end < sourceLength && (isLetter(input()[end]) || isDigit(input()[end]))) ++end;
    }
    advanceN(end - begin);
}

bool Scanner::handleSpecialChars() {
    if (isSpecialChar(current)){
        String accum;
        accum += current;
        while (hasNext() && isSpecialChar(current)) {
            if (handleSpecialChar(input()[position], current, accum)) {
                next();
            }
        }
        return true;
    }
    return false;
}

int Scanner::matchBlockStartIndex() const {
    int best = -1;
    size_t bestLen = 0;
    for (int i = 0; i < (int)commentCfg.blockPairs.size(); ++i) {
        const auto& st = commentCfg.blockPairs[i].start;
        if (matchAt(position, st) && st.size() > bestLen) {
            best = i;
            bestLen = st.size();
        }
    }
    return best;
}

bool Scanner::tryScanCommentDelimiter() {
    // Prefer starts over ends,
    if (tryScanCommentStart()) { return true; }
    if (tryScanCommentEnd())   { return true; }
    return false;
}
 
bool Scanner::inBounds(size_t pos) const { return pos < sourceLength; }

void Scanner::step() {
    const bool newline = current == '\n';
    next();
    if (newline) {
        line++;
        column = 1;
    }
}

bool Scanner::atLineEnd() const {
    return current == '\n' || (current == '\r' && position + 1 < sourceLength && input()[position + 1] == '\n');
}

bool Scanner::lineCommentMayStart() const {
    if (!commentCfg.lineStartsNeedSpace || position == textBegin) return true;
    const char prev = input()[position - 1];
    return prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r';
}

const QuoteConfig* Scanner::quoteConfig(char quote) const {
    for (const auto& q : commentCfg.quotes)
        if (q.quote == quote) return &q;
    return nullptr;
}

int Scanner::lineIndent(size_t pos) const {
    size_t start = pos;
    while (start > 0 && input()[start - 1] != '\n') --start;
    int indent = 0;
    while (start + indent < sourceLength && (input()[start + indent] == ' ' || input()[start + indent] == '\t')) ++indent;
    return indent;
}

const TextBlockConfig* Scanner::matchTextBlock() const {
    const TextBlockConfig* best = nullptr;
    for (const auto& block : commentCfg.textBlocks) {
        if (!matchAt(position, block.opener)) continue;
        if (best && block.opener.size() <= best->opener.size()) continue;
        if (block.endsLine) {
            if (position > 0 && input()[position - 1] != ' ' && input()[position - 1] != '\t' &&
                input()[position - 1] != '\n') continue;
            size_t after = position + block.opener.size();
            while (after < sourceLength && (input()[after] == ' ' || input()[after] == '\t' || input()[after] == '\r')) ++after;
            bool comment = false;
            for (const auto& start : commentCfg.lineStarts)
                comment = comment || (after > position + block.opener.size() && matchAt(after, start));
            if (after < sourceLength && input()[after] != '\n' && !comment) continue;
        }
        best = &block;
    }
    return best;
}

bool Scanner::tryScanTextBlock() {
    const TextBlockConfig* block = matchTextBlock();
    if (!block) return false;
    const int openerIndent = lineIndent(position);
    emitRange(RawKind::TextBlockStart, position, block->opener.size(), line, column);
    advanceN(block->opener.size());
    if (block->closer.empty()) scanIndentedText(openerIndent);
    else scanDelimitedText(block->closer);
    return true;
}

// The rest of the opener's line (whitespace, a comment), then each following
// line indented deeper than the opener's line as a TextLine. Blank lines
// inside the block are Newline counts; the newline after the last text line,
// and anything after it, is left to normal scanning.
void Scanner::scanIndentedText(int openerIndent) {
    while (current == ' ' || current == '\t' || current == '\r') next();
    tryScanComment();

    struct Line { size_t begin, end; };
    Vector<Line> lines;
    size_t at = position;  // a newline (or the end)
    while (at < sourceLength && input()[at] == '\n') {
        size_t begin = at + 1;
        int indent = 0;
        while (begin < sourceLength && (input()[begin] == ' ' || input()[begin] == '\t')) { ++begin; ++indent; }
        if (begin >= sourceLength) break;
        if (input()[begin] == '\n' || input()[begin] == '\r') {  // blank: part of the block if text follows
            at = input()[begin] == '\r' ? begin + 1 : begin;
            continue;
        }
        if (indent <= openerIndent) break;
        size_t end = begin;
        while (end < sourceLength && input()[end] != '\n') ++end;
        lines.push_back({begin, end});
        at = end;
    }

    for (const auto& text : lines) {
        const size_t nlBegin = position;
        const int nlLine = line, nlColumn = column;
        int newlines = 0;
        while (position < text.begin) {
            if (current == '\n') ++newlines;
            step();
        }
        emit(RawKind::Newline, nlBegin, position - nlBegin, nlLine, nlColumn, newlines,
             [] { return String("\n"); });
        size_t end = text.end;
        if (end > text.begin && input()[end - 1] == '\r') --end;
        emitRange(RawKind::TextLine, text.begin, end - text.begin, line, column);
        while (position < text.end) step();
    }
}

// Everything up to `closer`, one TextLine per line segment (an empty segment
// is only its Newline), then the closer as TextBlockEnd.
void Scanner::scanDelimitedText(const String& closer) {
    const int startLine = line, startColumn = column;
    size_t begin = position;
    int segLine = line, segColumn = column;
    while (true) {
        if (position >= sourceLength) throw ScannerError("Unclosed text block, expected " + closer, startLine, startColumn);
        const bool closing = matchAt(position, closer);
        if (closing || current == '\n') {
            size_t end = position;
            if (end > begin && input()[end - 1] == '\r') --end;
            if (end > begin) emitRange(RawKind::TextLine, begin, end - begin, segLine, segColumn);
            if (closing) {
                emitRange(RawKind::TextBlockEnd, position, closer.size(), line, column);
                advanceN(closer.size());
                return;
            }
            const int nlLine = line, nlColumn = column;
            const size_t nlBegin = position;
            int newlines = 0;
            while (current == '\n') { ++newlines; step(); }
            emit(RawKind::Newline, nlBegin, position - nlBegin, nlLine, nlColumn, newlines,
                 [] { return String("\n"); });
            begin = position;
            segLine = line;
            segColumn = column;
            continue;
        }
        step();
    }
}

bool Scanner::matchAt(size_t pos, const String& s) const {
    if (s.empty()) {return false;}
    if (pos >= sourceLength || s.size() > sourceLength - pos) {return false;}
    if (input()[pos] != s.front()) return false;
    return input().compare(pos, s.size(), s) == 0;
}

void Scanner::advanceN(size_t n) {
    // Delimiters and single-line spans advance columns only; step() counts newlines.
    if (position < sourceLength) {
        const size_t count = std::min(n, sourceLength - position);
        position += count;
        column += static_cast<int>(count);
    }
    current = (position < sourceLength) ? input()[position] : '\0';
}


bool Scanner::tryScanCommentStart() {
    if (!inBounds(position)) return false;

    // Find the best (longest) match among ALL start delimiters at this position.
    enum class StartKind { None, Line, Block };
    StartKind bestKind = StartKind::None;
    size_t bestLen = 0;
    int bestBlockIndex = -1;
    String bestLexeme;

    // 1) Line starts
    const bool lineMayStart = lineCommentMayStart();
    for (const auto& ls : commentCfg.lineStarts) {
        if (lineMayStart && ls.size() >= bestLen && matchAt(position, ls)) {
            // Longest match wins; if equal length, line vs block tie-break is arbitrary.
            bestKind = StartKind::Line;
            bestLen = ls.size();
            bestLexeme = ls;
            bestBlockIndex = -1;
        }
    }

    // 2) Block starts
    for (int i = 0; i < (int)commentCfg.blockPairs.size(); ++i) {
        const auto& bs = commentCfg.blockPairs[i].start;
        if (bs.size() >= bestLen && matchAt(position, bs)) {
            bestKind = StartKind::Block;
            bestLen = bs.size();
            bestLexeme = bs;
            bestBlockIndex = i;
        }
    }

    if (bestKind == StartKind::None) {return false;}

    const int startLine = line;
    const int startCol  = column;

    if (bestKind == StartKind::Line) {
        rawTokens.emplace_back(RawKind::CommentLineStart, bestLexeme, startLine, startCol, -1);
        advanceN(bestLen);
        return true;
    }

    rawTokens.emplace_back(RawKind::CommentBlockStart, bestLexeme, startLine, startCol, bestBlockIndex);
    advanceN(bestLen);
    return true;
}

bool Scanner::tryScanCommentEnd() {
    if (!inBounds(position)) return false;
    size_t bestLen = 0;
    int bestBlockIndex = -1;
    String bestLexeme;

    for (int i = 0; i < (int)commentCfg.blockPairs.size(); ++i) {
        const auto& be = commentCfg.blockPairs[i].end;
        if (be.size() >= bestLen && matchAt(position, be)) {
            bestLen = be.size();
            bestLexeme = be;
            bestBlockIndex = i;
        }
    }

    if (bestBlockIndex < 0) return false;

    const int endLine = line;
    const int endCol  = column;

    rawTokens.emplace_back(RawKind::CommentBlockEnd, bestLexeme, endLine, endCol, bestBlockIndex);
    advanceN(bestLen);
    return true;
}

const char* preprocessorKindToString(PreprocessorKind k) {
    switch (k) {
        case PreprocessorKind::None:  return "None";
        case PreprocessorKind::If:    return "If";
        case PreprocessorKind::Elif:  return "Elif";
        case PreprocessorKind::Else:  return "Else";
        case PreprocessorKind::Endif: return "Endif";
        case PreprocessorKind::Other: return "Other";
        default: return "<PreprocessorKind?>";
    }
}

const char* rawKindToString(RawKind k) {
    switch (k) {
        case RawKind::SOF: return "SOF";
        case RawKind::EOF_: return "EOF_";
        case RawKind::Newline: return "Newline";
        case RawKind::Tab: return "Tab";
        case RawKind::Space: return "Space";
        case RawKind::Semicolon: return "Semicolon";

        case RawKind::CommentLineStart: return "CommentLineStart";
        case RawKind::CommentBlockStart: return "CommentBlockStart";
        case RawKind::CommentBlockEnd: return "CommentBlockEnd";

        case RawKind::Identifier: return "Identifier";
        case RawKind::Number: return "Number";
        case RawKind::String: return "String";
        case RawKind::Char: return "Char";
        case RawKind::Text: return "Text";

        case RawKind::Operator: return "Operator";
        case RawKind::Punctuation: return "Punctuation";
        case RawKind::Preprocessor: return "Preprocessor";
        case RawKind::SpecialChar: return "SpecialChar";
        case RawKind::Unknown: return "Unknown";
        case RawKind::NoOp: return "NoOp";
        case RawKind::Comment: return "Comment";
        case RawKind::Indent: return "Indent";
        case RawKind::Dedent: return "Dedent";
        case RawKind::TextBlockStart: return "TextBlockStart";
        case RawKind::TextLine: return "TextLine";
        case RawKind::TextBlockEnd: return "TextBlockEnd";
        default: return "<RawKind?>";
    }
}

bool Scanner::matchBlockEndIndex(int i) const {
    return matchAt(position, commentCfg.blockPairs[i].end);
}

RawToken Scanner::scanBlockComment(int pairIndex) {
    const int startLine = line;
    const int startCol  = column;

    const auto& pair = commentCfg.blockPairs[pairIndex];
    advanceN(pair.start.size());

    String text;
    int depth = 1;

    while (position < sourceLength) {
        // If nestable, detect nested start
        if (pair.nestable) {
            int nested = matchBlockStartIndex();
            if (nested == pairIndex) {
                advanceN(commentCfg.blockPairs[nested].start.size());
                depth++;
                continue;
            }
        }

        // Detect end for this pair
        if (matchBlockEndIndex(pairIndex)) {
            advanceN(pair.end.size());
            depth--;
            if (depth == 0) {
                return RawToken(RawKind::Comment, text, startLine, startCol, pairIndex);
            }
            continue;
        }

        // Normal char in comment body
        text += current;
        next(); // use next() for newline tracking
    }

    throw ScannerError("Unterminated block comment", startLine, startCol);
}

RawToken Scanner::scanLineComment(const String& startLexeme) {
    const int startLine = line;
    const int startCol  = column;

    advanceN(startLexeme.size());

    String text;
    while (position < sourceLength && !atLineEnd()) {
        text += current;
        next();
    }
    if (!text.empty() && text[0] == ' ') text.erase(0, 1);

    return RawToken(RawKind::Comment, text, startLine, startCol, -1);
}

bool Scanner::tryScanComment() {
    if (!inBounds(position)) return false;

    enum class StartKind { None, Line, Block };
    StartKind bestKind = StartKind::None;
    size_t bestLen = 0;
    int bestBlockIndex = -1;

    const bool lineMayStart = lineCommentMayStart();
    for (const auto& ls : commentCfg.lineStarts) {
        if (lineMayStart && matchAt(position, ls) && ls.size() > bestLen) {
            bestKind = StartKind::Line;
            bestLen = ls.size();
            bestBlockIndex = -1;
        }
    }
    for (int i = 0; i < (int)commentCfg.blockPairs.size(); ++i) {
        const auto& bs = commentCfg.blockPairs[i].start;
        if (matchAt(position, bs) && bs.size() > bestLen) {
            bestKind = StartKind::Block;
            bestLen = bs.size();
            bestBlockIndex = i;
        }
    }

    if (bestKind == StartKind::None) return false;

    const int startLine = line;
    const int startCol  = column;
    const size_t begin = position;

    if (bestKind == StartKind::Line) {
        advanceN(bestLen);
        const size_t bodyBegin = position;
        const int bodyLine = line, bodyColumn = column;
        const auto body = consumeLineCommentBody();
        if (commentCfg.skipComments) return true;
        emitRange(RawKind::CommentLineStart, begin, bestLen, startLine, startCol);
        emit(RawKind::Comment, bodyBegin, position - bodyBegin, bodyLine, bodyColumn, -1,
             [body] { return String(body); });
        return true;
    }

    advanceN(bestLen);
    const size_t bodyBegin = position;
    const int bodyLine = line, bodyColumn = column;
    std::optional<String> body;
    if (commentCfg.skipComments || indexedOutput_) {
        consumeBlockCommentBody<false>(bestBlockIndex, nullptr);
    } else {
        body.emplace();
        consumeBlockCommentBody<true>(bestBlockIndex, &*body);
    }
    if (commentCfg.skipComments) return true;
    const size_t endLength = commentCfg.blockPairs[bestBlockIndex].end.size();
    const size_t endBegin = position - endLength;
    emitRange(RawKind::CommentBlockStart, begin, bestLen, startLine, startCol, bestBlockIndex);
    emit(RawKind::Comment, bodyBegin, endBegin - bodyBegin, bodyLine, bodyColumn, bestBlockIndex,
         [&] { return std::move(*body); });
    emitRange(RawKind::CommentBlockEnd, endBegin, endLength, line, column, bestBlockIndex);
    return true;
}

bool Scanner::tryScanPreprocessorDirective() {
    const auto& ppCfg = commentCfg.preprocessor;
    if (ppCfg.marker.empty()) return false;
    if (!matchAt(position, ppCfg.marker)) return false;

    size_t probe = position + ppCfg.marker.size();
    while (probe < sourceLength && (input()[probe] == ' ' || input()[probe] == '\t')) ++probe;

    const size_t keyStart = probe;
    while (probe < sourceLength && (std::isalpha(static_cast<unsigned char>(input()[probe])) || input()[probe] == '_')) {
        ++probe;
    }
    const auto key = input().substr(keyStart, probe - keyStart);

    for (const auto& entry : ppCfg.keys) {
        if (entry.key != key) continue;

        const int startLine = line;
        const int startCol = column;
        const size_t begin = position;
        advanceN(probe - position);
        emit(RawKind::Preprocessor, begin, probe - begin, startLine, startCol, -1,
             [&] { return ppCfg.marker + String(key); }, entry.kind);
        return true;
    }

    return false;
}

RawToken Scanner::scanLineCommentBody() {
    const int startLine = line;
    const int startCol  = column;
    const auto body = consumeLineCommentBody();
    return RawToken(RawKind::Comment, String(body), startLine, startCol, -1);
}

std::string_view Scanner::consumeLineCommentBody() {
    size_t begin = position;
    size_t end = std::min(input().find('\n', begin), sourceLength);
    if (end < sourceLength && end > begin && input()[end - 1] == '\r') --end;
    advanceN(end - begin);
    if (begin < end && input()[begin] == ' ') ++begin;
    return input().substr(begin, end - begin);
}

RawToken Scanner::scanBlockCommentBody(int pairIndex) {
    const int startLine = line;
    const int startCol  = column;
    String text;
    consumeBlockCommentBody<true>(pairIndex, &text);
    return RawToken(RawKind::Comment, std::move(text), startLine, startCol, pairIndex);
}

template <bool KeepText>
void Scanner::consumeBlockCommentBody(int pairIndex, String* text) {
    const int startLine = line;
    const int startCol  = column;
    const auto& pair = commentCfg.blockPairs[pairIndex];

    size_t begin = position;
    int depth = 1;

    while (position < sourceLength) {
        // nested start
        if (pair.nestable && matchAt(position, pair.start)) {
            if constexpr (KeepText) text->append(input(), begin, position - begin);
            advanceN(pair.start.size());
            begin = position;
            depth++;
            continue;
        }

        // end
        if (matchAt(position, pair.end)) {
            if constexpr (KeepText) text->append(input(), begin, position - begin);
            advanceN(pair.end.size());
            begin = position;
            depth--;
            if (depth == 0) {
                return;
            }
            continue;
        }

        next();
    }

    throw ScannerError("Unterminated block comment", startLine, startCol);
}

String escapeLexeme(const String& s) {
    String out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    // Other control chars
                    out += "\\x";
                    const char* hex = "0123456789ABCDEF";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void printRawTokens(const Vector<RawToken>& toks, std::ostream& os) {
    os << "RawTokens (" << toks.size() << ")\n";
    os << "------------------------------------------------------------\n";
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        os << std::setw(4) << i << "  "
           << std::setw(18) << rawKindToString(t.kind)
           << "  (" << t.line << ":" << t.column << ")";

        if (t.aux != -1) {
            os << "  aux=" << t.aux;
        }

        if (t.kind == RawKind::Preprocessor) {
            os << "  ppKind=" << preprocessorKindToString(t.ppKind);
        }

        if (!t.lexeme.empty()) {
            os << "  \"" << escapeLexeme(t.lexeme) << "\"";
        }

        os << "\n";
    }
    os << "------------------------------------------------------------\n";
}

std::ostream& operator<<(std::ostream& os, const RawToken& t) {
    os << rawKindToString(t.kind) << " (" << t.line << ":" << t.column << ")";
    if (t.aux != -1) os << " aux=" << t.aux;
    if (t.kind == RawKind::Preprocessor) os << " ppKind=" << preprocessorKindToString(t.ppKind);
    if (!t.lexeme.empty()) os << " \"" << escapeLexeme(t.lexeme) << "\"";
    return os;
}

ScannerError::ScannerError(const String& m, int l, int c): RunTimeError(m), message(m), line(l), column(c) {}

String ScannerError::errorString() const {
    return "ScannerError: " + message + " on Line: " + std::to_string(line) + ",  Column: " + std::to_string(column);
}

const char* ScannerError::what() const noexcept {
    cache = errorString();
    return cache.c_str();
};
