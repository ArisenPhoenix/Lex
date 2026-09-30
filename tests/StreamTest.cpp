// SPDX-License-Identifier: Apache-2.0
#include "lex/Scanner.hpp"

#include <limits>
#include <clocale>
#include <random>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<IndexedToken>);

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void spellingAndOwnership() {
    String source = "\xEF\xBB\xBF" "word  12.3 ++ \"a\\nb\" // note\r\n# if YES";
    Vector<IndexedToken> tokens;
    {
        Scanner scanner;
        scanner.commentCfg.lineStarts = {"//"};
        scanner.commentCfg.preprocessor = {"#", {{"if", PreprocessorKind::If}}};
        tokens = scanner.stream(source);
        check(scanner.source.empty() && scanner.rawTokens.empty(), "stream must not own source or RawTokens");
        check(scanner.position == 0 && scanner.sourceLength == 0 && !scanner.hasNext() &&
              !scanner.followsWordChar(), "Streaming cursor must release the borrowed input");
    }
    const Vector<std::string_view> spellings = {
        "", "word", "  ", "12.3", " ", "++", " ", "\"a\\nb\"", " ", "//", " note", "\r\n", "# if", " ", "YES", ""
    };
    check(tokens.size() == spellings.size(), "Unexpected stream token count");
    size_t offset = 3;
    for (size_t i = 0; i < tokens.size(); ++i) {
        check(tokens[i].text(source) == spellings[i], "Original spelling lost");
        if (i != 0) {
            check(tokens[i].offset == offset, "Incorrect byte offset");
            check(tokens[i].length == spellings[i].size(), "Incorrect byte length");
            offset += tokens[i].length;
        }
    }
    check(tokens[5].aux == 2 && tokens[12].ppKind == PreprocessorKind::If, "Token metadata lost");
    check(tokens[1].column == 4 && tokens[12].line == 2, "BOM/CRLF positions differ");

    String relocated = source;
    source.clear();
    source.shrink_to_fit();
    check(tokens[7].text(relocated) == "\"a\\nb\"", "Indices must survive source relocation");
    check(relocated.substr(tokens[7].offset, tokens[7].length) == tokens[7].text(relocated), "Manual slicing differs");
    for (const IndexedToken invalid : {IndexedToken{.offset = 100, .length = 1, .kind = RawKind::Text},
                                      IndexedToken{.offset = 0, .length = std::numeric_limits<size_t>::max(),
                                                   .kind = RawKind::Text}}) {
        bool threw = false;
        try { invalid.text("short"); } catch (const std::out_of_range&) { threw = true; }
        check(threw, "Out-of-range indices accepted");
    }
}

void reuseAndModes() {
    Scanner scanner;
    const auto first = scanner.stream("first");
    const auto empty = scanner.stream({});
    check(empty.size() == 2 && empty[0].offset == 0 && empty[1].offset == 0, "Empty input differs");
    scanner.commentCfg.lineStarts = {"#"};
    scanner.commentCfg.skipComments = true;
    const auto skipped = scanner.stream("# skip\nnext");
    check(skipped.size() == 4 && skipped[2].text("# skip\nnext") == "next", "Updated config ignored");
    bool threw = false;
    try { scanner.stream("\"bad\\q\""); } catch (const ScannerError&) { threw = true; }
    check(threw, "Streaming must still validate escapes");
    const auto next = scanner.stream("other");
    check(next[1].offset == 0 && next[1].line == 1 && next[1].column == 1, "State not reset after error");
    check(first[1].text("first") == "first", "Reuse invalidated earlier tokens");

    const char bounded[] = {'a', '\0', 'b', 'x'};
    const auto boundedTokens = scanner.stream(std::string_view(bounded, 3));
    check(boundedTokens.size() == 5 && boundedTokens[2].text(std::string_view(bounded, 3))[0] == '\0' &&
          boundedTokens.back().offset == 3, "Non-terminated input or embedded NUL mishandled");

    threw = false;
    try { scanner.scan(); } catch (const std::logic_error&) { threw = true; }
    check(threw, "Default scanner must use stream");
    Scanner owning("\"a\\nb\"", {});
    threw = false;
    try { owning.stream("other"); } catch (const std::logic_error&) { threw = true; }
    check(threw, "Source-taking scanner must use scan");
    const auto legacy = owning.scan();
    check(legacy[1].lexeme == "a\nb" && owning.rawTokens[1].lexeme == "a\nb", "Legacy lexeme changed");
}

void compareModes(const String& source, const CommentConfig& cfg) {
    Vector<RawToken> legacy;
    Vector<IndexedToken> indexed;
    String legacyError, indexedError;
    try { legacy = Scanner(source, cfg).scan(); }
    catch (const ScannerError& e) { legacyError = e.what(); }
    Scanner scanner;
    scanner.commentCfg = cfg;
    try { indexed = scanner.stream(source); }
    catch (const ScannerError& e) { indexedError = e.what(); }
    check(legacyError == indexedError, "Stream and scan errors differ");
    if (!legacyError.empty()) return;
    check(legacy.size() == indexed.size(), "Stream and scan token counts differ");
    for (size_t i = 0; i < legacy.size(); ++i) {
        const auto& a = legacy[i];
        const auto& b = indexed[i];
        check(a.kind == b.kind && a.line == b.line && a.column == b.column &&
              a.aux == b.aux && a.ppKind == b.ppKind, "Stream and scan metadata differ");
        b.text(source); // also validate every range
        if (a.kind == RawKind::Identifier || a.kind == RawKind::Number || a.kind == RawKind::TextLine)
            check(a.lexeme == b.text(source), "Unmodified token spelling differs");
    }
}

void configuredInputs() {
    CommentConfig cfg;
    cfg.lineStarts = {"//", "#"};
    cfg.blockPairs = {{"/*", "*/", true}, {"{-", "-}", true}};
    cfg.textBlocks = {{"'''", "'''", false}, {"|", "", true}};
    cfg.stringPrefixes = {{"r", true, {'"', true, true, false}}};
    cfg.preprocessor = {"#", {{"if", PreprocessorKind::If}}};
    for (const auto& source : {"/*outer/*inner*/end*/x", "a: |\n    text\n\n    more\nend",
                              "'''a\r\nb'''x", "r\"\\d\"", "# if VALUE", "/*unfinished"})
        compareModes(source, cfg);
    std::mt19937 random(2049);
    const Vector<String> pieces = {"alpha_1", "123.4.5", " ", "\t", "\r\n", "\n", "{", "}}", "[", "]",
        "/*x/*y*/z*/", "// comment\n", "# if X\n", "- ", "'it''s'", "\"a\\n\\x41\\101b\"", "r\"\\d\"",
        "\"a\nb\"", "\"bad\\q\"", "'''text\nmore'''", "|\n    text\n", String(2048, 'x')};
    for (int test = 0; test < 500; ++test) {
        cfg.skipComments = test % 3 == 0;
        cfg.collapseDuplicates = test % 4 != 0;
        cfg.lineStartsNeedSpace = test % 2;
        cfg.quotesNeedWordBoundary = test % 3 == 0;
        cfg.quotes = {{'"', bool(test & 1), bool(test & 2), bool(test & 4)},
                      {'\'', bool(test & 8), bool(test & 16), bool(test & 32)}};
        String source;
        for (int i = 0; i < 20; ++i) source += pieces[random() % pieces.size()];
        compareModes(source, cfg);
    }
}

void classifierDispatch() {
    Scanner scanner;
    // Each delimiter can start with any byte, including NUL and high bytes.
    // Whitespace is consumed before delimiter recognition, as in scan().
    for (unsigned byte = 0; byte < 256; ++byte) {
        if (byte == ' ' || byte == '\t' || byte == '\n') continue;
        const String marker = String(1, static_cast<char>(byte)) + "marker";
        const String source = marker + " body";
        scanner.commentCfg = {};
        scanner.commentCfg.lineStarts = {marker};
        auto tokens = scanner.stream(source);
        check(tokens.size() == 4 && tokens[1].kind == RawKind::CommentLineStart &&
              tokens[1].text(source) == marker, "Comment first-byte dispatch differs");

        scanner.commentCfg = {};
        scanner.commentCfg.preprocessor = {marker, {{"body", PreprocessorKind::Other}}};
        tokens = scanner.stream(source);
        check(tokens.size() == 3 && tokens[1].kind == RawKind::Preprocessor,
              "Preprocessor first-byte dispatch differs");

        scanner.commentCfg = {};
        scanner.commentCfg.textBlocks = {{marker, "!", false}};
        tokens = scanner.stream(source + "!");
        check(tokens.size() == 5 && tokens[1].kind == RawKind::TextBlockStart &&
              tokens[3].kind == RawKind::TextBlockEnd, "Text-block first-byte dispatch differs");
    }

    scanner.commentCfg = {};
    scanner.commentCfg.lineStarts = {"a", "alpha"};
    scanner.commentCfg.preprocessor = {"alpha", {{"", PreprocessorKind::Other}}};
    scanner.commentCfg.textBlocks = {{"alpha", "!", false}};
    auto tokens = scanner.stream("alpha!");
    check(tokens[1].kind == RawKind::CommentLineStart && tokens[1].length == 5,
          "Longest comment match or comment precedence changed");
    scanner.commentCfg.lineStarts.clear();
    check(scanner.stream("alpha!")[1].kind == RawKind::Preprocessor, "Preprocessor precedence changed");
    scanner.commentCfg.preprocessor = {};
    check(scanner.stream("alpha!")[1].kind == RawKind::TextBlockStart, "Text-block precedence changed");
    scanner.commentCfg.textBlocks.clear();
    check(scanner.stream("alpha!")[1].kind == RawKind::Identifier, "Removed delimiter flags retained");
    scanner.commentCfg.blockPairs = {{"/*", "*/", false}};
    tokens = scanner.stream("/word");
    check(tokens[1].kind == RawKind::Operator && tokens[2].kind == RawKind::Identifier,
          "Failed delimiter match must fall back to ordinary classification");

    const String savedLocale = std::setlocale(LC_CTYPE, nullptr);
    scanner.commentCfg = {};
    for (const char* locale : {"C", ""}) {
        if (!std::setlocale(LC_CTYPE, locale)) continue;
        for (unsigned byte = 128; byte < 256; ++byte) {
            const char c = static_cast<char>(byte);
            const String source = String(1, c) + "a1";
            tokens = scanner.stream(source);
            const bool letter = scanner.isLetter(c);
            check(tokens[1].kind == (letter ? RawKind::Identifier : RawKind::Unknown) &&
                  tokens[1].length == (letter ? 3u : 1u), "Locale-sensitive byte classification changed");
        }
    }
    std::setlocale(LC_CTYPE, savedLocale.c_str());
}
}

int main() {
    try {
        spellingAndOwnership();
        reuseAndModes();
        configuredInputs();
        classifierDispatch();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
