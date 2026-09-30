// SPDX-License-Identifier: Apache-2.0
#include "lex/Structurizer.hpp"

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void same(const Vector<RawToken>& got, const Vector<RawToken>& want) {
    check(got.size() == want.size(), "Token count differs");
    for (size_t i = 0; i < got.size(); ++i) {
        const auto& a = got[i];
        const auto& b = want[i];
        check(a.kind == b.kind && a.lexeme == b.lexeme && a.line == b.line &&
              a.column == b.column && a.aux == b.aux && a.ppKind == b.ppKind,
              "Token fields differ");
    }
}

void spansAndPositions() {
    const String word(4096, 'a');
    const String digits(4096, '7');
    Scanner scanner(word + " " + digits + ".25.\r\n\"plain\ntext\\nend\" tail", {});
    const auto tokens = scanner.scan();
    same(tokens, {
        RawToken(RawKind::SOF),
        RawToken(RawKind::Identifier, word, 1, 1),
        RawToken(RawKind::Space, " ", 1, 4097, 1),
        RawToken(RawKind::Number, digits + ".25", 1, 4098),
        RawToken(RawKind::Punctuation, ".", 1, 8197, 1),
        RawToken(RawKind::Newline, "\n", 1, 8198, 1),
        RawToken(RawKind::String, "plain\ntext\nend", 2, 1, -1),
        RawToken(RawKind::Space, " ", 3, 11, 1),
        RawToken(RawKind::Identifier, "tail", 3, 12, -1),
        RawToken(RawKind::EOF_, "EOF", 3, 16, -1),
    });
    same(scanner.rawTokens, tokens);
    check(scanner.position == scanner.source.size() && scanner.current == '\0' &&
          scanner.line == 3 && scanner.column == 16, "Scanner end state differs");

    const String embedded("a\0b", 3);
    const auto text = Scanner("\"" + embedded + "\"", {}).scan();
    check(text[1].lexeme == embedded, "Embedded NUL lost");
}

void commentBodies() {
    CommentConfig cfg;
    cfg.lineStarts = {"/", "//"};
    cfg.blockPairs = {{"/*", "*/", true}};
    const String body(4096, 'c');
    const String source = "// " + body + "\r\n/*outer/*inner*/end*/x";
    const auto kept = Scanner(source, cfg).scan();
    check(kept[1].lexeme == "//" && kept[2].lexeme == body &&
          kept[3].kind == RawKind::Newline && kept[3].column == 4100,
          "Line comment boundary differs");
    check(kept[5].lexeme == "outerinnerend", "Nested comment body differs");
    cfg.skipComments = true;
    const auto skipped = Scanner(source, cfg).scan();
    Vector<RawToken> filtered;
    for (const auto& t : kept) {
        if (t.kind != RawKind::Comment && t.kind != RawKind::CommentLineStart &&
            t.kind != RawKind::CommentBlockStart && t.kind != RawKind::CommentBlockEnd)
            filtered.push_back(t);
    }
    same(skipped, filtered);

    Scanner line("  text\rrest\r\nnext", cfg);
    const auto lineBody = line.scanLineCommentBody();
    check(lineBody.lexeme == " text\rrest" && line.current == '\r' && line.column == 12,
          "Public line body reader must retain text even with skipComments");
    Scanner block("outer/*inner*/end*/tail", cfg);
    check(block.scanBlockCommentBody(0).lexeme == "outerinnerend" && block.current == 't',
          "Public block body reader must retain text even with skipComments");
    for (bool skip : {false, true}) {
        cfg.skipComments = skip;
        bool threw = false;
        try { Scanner("/*unfinished", cfg).scan(); }
        catch (const ScannerError&) { threw = true; }
        check(threw, "Unterminated skipped comments must still throw");
    }
    Scanner mutableConfig("# text", {});
    mutableConfig.commentCfg.lineStarts = {"#"};
    check(mutableConfig.scan()[1].kind == RawKind::CommentLineStart, "Config edit ignored");
}

void publicReaders() {
    using Reader = RawToken (Scanner::*)();
    auto verify = [](const String& source, Reader reader, RawKind kind, const String& lexeme,
                     int aux, size_t consumed, bool collapse = true) {
        CommentConfig cfg;
        cfg.collapseDuplicates = collapse;
        Scanner scanner(source, cfg);
        const auto token = (scanner.*reader)();
        check(token.kind == kind && token.lexeme == lexeme && token.aux == aux &&
              token.line == 1 && token.column == 1 && token.ppKind == PreprocessorKind::None,
              "Public reader token changed");
        check(scanner.position == consumed && scanner.column == static_cast<int>(consumed) + 1 &&
              scanner.rawTokens.empty(), "Public reader must advance without emitting");
    };
    const String word(4096, 'a');
    verify(word + "_1!", &Scanner::readIdentifier, RawKind::Identifier, word + "_1", -1, 4098);
    verify("12.34.5", &Scanner::readNumber, RawKind::Number, "12.34", -1, 5);
    verify("+++x", &Scanner::readOperator, RawKind::Operator, "+", 3, 3);
    verify("+++x", &Scanner::readOperator, RawKind::Operator, "+", 1, 1, false);
    verify(":::x", &Scanner::readPunctuation, RawKind::Punctuation, ":", 3, 3);
    verify(":::x", &Scanner::readPunctuation, RawKind::Punctuation, ":", 1, 1, false);
    for (Reader reader : {&Scanner::readIdentifier, &Scanner::readNumber,
                          &Scanner::readOperator, &Scanner::readPunctuation}) {
        Scanner scanner(" ", {});
        bool threw = false;
        try { (scanner.*reader)(); } catch (const ScannerError&) { threw = true; }
        check(threw && scanner.position == 0 && scanner.rawTokens.empty(), "Reader validation changed");
    }
    Scanner text("\"a\\nb\"tail", {});
    check(text.readText().lexeme == "a\nb" && text.position == 6 && text.rawTokens.empty(),
          "Public text reader changed");
    QuoteConfig raw{'"', true, true, false};
    Scanner overridden("\"\\d\"", {});
    check(overridden.readText(&raw).lexeme == "\\d", "Explicit quote override changed");
}

void delimiterLookups() {
    LayoutConfig cfg;
    cfg.scopeMode = LayoutConfig::ScopeMode::Braces;
    cfg.scopeOpeners = {"{", "BEGIN"};
    cfg.scopeClosers = {"}", "END"};
    const Vector<RawToken> input = {
        RawToken(RawKind::SOF),
        RawToken(RawKind::Punctuation, "BEGIN", 1, 1, -1),
        RawToken(RawKind::Punctuation, "{", 1, 7, 2),
        RawToken(RawKind::Punctuation, "}", 1, 9, 2),
        RawToken(RawKind::Punctuation, "END", 1, 11, -1),
        RawToken(RawKind::EOF_, "EOF", 1, 14, -1),
    };
    const Vector<RawToken> expected = {
        input[0], input[1], RawToken(RawKind::Indent, "", 1, 1, 1),
        input[2], RawToken(RawKind::Indent, "", 1, 7, 3),
        RawToken(RawKind::Dedent, "", 1, 9, 1), input[3],
        RawToken(RawKind::Dedent, "", 1, 11, 0), input[4], input[5],
    };
    Structurizer layout(cfg);
    same(layout.structurize(input), expected);
    same(layout.structurize(input), expected);
    cfg.scopeOpeners.push_back("}"); // closers take precedence in brace mode
    same(Structurizer(cfg).structurize(input), expected);

    cfg = {};
    cfg.bracketOpeners = {"BEGIN", "["};
    cfg.bracketClosers = {"END", "]", "["}; // openers take precedence here
    for (const auto& pair : Vector<std::pair<String, String>>{{"BEGIN", "END"}, {"[", "]"}}) {
        const Vector<RawToken> bracketed = {
            RawToken(RawKind::Punctuation, pair.first, 1, 1, 2),
            RawToken(RawKind::Newline, "\n", 1, 2, 1),
            RawToken(RawKind::Space, " ", 2, 1, 4),
            RawToken(RawKind::Identifier, "x", 2, 5, -1),
            RawToken(RawKind::Punctuation, pair.second, 2, 6, 2),
        };
        same(Structurizer(cfg).structurize(bracketed), {bracketed[0], bracketed[3], bracketed[4]});
    }
}
}

int main() {
    try {
        spansAndPositions();
        commentBodies();
        publicReaders();
        delimiterLookups();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
