// SPDX-License-Identifier: Apache-2.0
#include "lex/Structurizer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace {
size_t checksum = 0;

template <typename F>
double measure(int iterations, F run) {
    for (int i = 0; i < 3; ++i) checksum += run();
    Vector<double> samples;
    for (int sample = 0; sample < 5; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) checksum += run();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        samples.push_back(std::chrono::duration<double, std::milli>(elapsed).count() / iterations);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void benchmark(const char* name, const String& unit, CommentConfig scannerCfg,
               LayoutConfig layoutCfg, int iterations) {
    String source;
    while (source.size() < 256 * 1024) source += unit;
    layoutCfg.scannerConfig = scannerCfg;
    const double combined = measure(iterations, [&] {
        return Structurizer(layoutCfg).structurize(Scanner(source, scannerCfg).scan()).size();
    });
    const double scan = measure(iterations, [&] {
        return Scanner(source, scannerCfg).scan().size();
    });
    const auto input = Scanner(source, scannerCfg).scan();
    const double layout = measure(iterations, [&] {
        return Structurizer(layoutCfg).structurize(input).size();
    });
    Scanner streaming;
    streaming.commentCfg = scannerCfg;
    const double indexed = measure(iterations, [&] {
        return streaming.stream(source).size();
    });
    std::cout << name << "," << source.size() << "," << input.size() << ","
              << scan << "," << layout << "," << combined << "," << indexed << '\n';
}
}

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 10;
    std::cout << "workload,bytes,tokens,scan_ms,layout_ms,combined_ms,stream_ms\n";
    CommentConfig code;
    code.lineStarts = {"//"};
    code.blockPairs = {{"/*", "*/", false}};
    code.preprocessor.marker = "#";
    code.preprocessor.keys = {{"if", PreprocessorKind::If}, {"endif", PreprocessorKind::Endif}};
    LayoutConfig braces;
    braces.scopeMode = LayoutConfig::ScopeMode::Braces;
    benchmark("code", "#if FEATURE\nint calculate_total(int input_value) {\n"
              "    // explain this calculation\n    return input_value + 123.45;\n}\n#endif\n",
              code, braces, iterations);
    CommentConfig python;
    python.lineStarts = {"#"};
    benchmark("indent", "def calculate_total(input_value):\n    if input_value:\n"
              "        return [input_value, 123.45]\n    return 0\n", python, {}, iterations);
    CommentConfig yaml;
    yaml.lineStarts = {"#"};
    yaml.lineStartsNeedSpace = true;
    yaml.quotesNeedWordBoundary = true;
    yaml.quotes = {{'"', true, true, false}, {'\'', false, false, true}};
    yaml.textBlocks = {{"|", "", true}, {">", "", true}};
    LayoutConfig items;
    items.indentMarkers = {"-"};
    benchmark("yaml", "- name: 'it''s a value'\n  values: [1, 2, 3]\n  text: |\n"
              "    some plain text\n    another line of text\n", yaml, items, iterations);
    const String longText(1024, 'x');
    benchmark("long_tokens", "long_identifier_" + longText + " = \"" + longText + "\";\n",
              code, braces, iterations);
    code.skipComments = true;
    benchmark("skip_comments", "// " + longText + "\n/* " + longText + " */ value;\n",
              code, braces, iterations);
    std::cerr << "checksum=" << checksum << '\n';
}
