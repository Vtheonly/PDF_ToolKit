// Zero-copy lexer tests (audit issue-1/task-2.3).
//
// Coverage: every token type round-trips; whitespace (incl. NUL/FF) and
// comments are skipped via the LUT; strings carry escapes and balanced
// parens; hex strings disambiguate from "<<"; names keep #-escapes
// raw; numbers parse (int/real classification, '+', saturation); the
// stream/endstream contract (caller owns payload positioning) holds;
// tolerant skips never recurse; lexing is deterministic on hostile
// input. The throughput gate lives in pdtk_bench_lexer (U-012 ratio).

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pdftoolkit/parser/lexer.hpp"

namespace {

using pdftoolkit::parser::PdfToken;
using pdftoolkit::parser::ZeroCopyLexer;

std::vector<std::uint8_t> operator""_b(const char* literal, std::size_t n) {
    return std::vector<std::uint8_t>(literal, literal + n);
}

int t(PdfToken::Type ty) { return static_cast<int>(ty); }

// Lexes everything; returns the token list (for assertions).
std::vector<PdfToken> lex_all(const std::vector<std::uint8_t>& bytes) {
    ZeroCopyLexer lexer(bytes);
    std::vector<PdfToken> out;
    while (true) {
        PdfToken tok = lexer.next();
        if (tok.type == PdfToken::Type::EndOfFile) {
            break;
        }
        out.push_back(tok);
    }
    return out;
}

PDTK_TEST(all_token_types_round_trip) {
    const auto doc = "1 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox "
                     "[0 0 612 792] >>\nstream\nendstream\nendobj\n"_b;
    const auto toks = lex_all(doc);
    // 1 0 obj << /Type /Page /Parent 2 0 R /MediaBox [ 0 0 612 792 ] >>
    // stream endstream endobj
    const int expected[] = {
        t(PdfToken::Type::Integer),      t(PdfToken::Type::Integer),
        t(PdfToken::Type::Keyword),      t(PdfToken::Type::DictStart),
        t(PdfToken::Type::Name),         t(PdfToken::Type::Name),
        t(PdfToken::Type::Name),         t(PdfToken::Type::Integer),
        t(PdfToken::Type::Integer),      t(PdfToken::Type::Keyword),
        t(PdfToken::Type::Name),         t(PdfToken::Type::ArrayStart),
        t(PdfToken::Type::Integer),      t(PdfToken::Type::Integer),
        t(PdfToken::Type::Integer),      t(PdfToken::Type::Integer),
        t(PdfToken::Type::ArrayEnd),     t(PdfToken::Type::DictEnd),
        t(PdfToken::Type::StreamStart),  t(PdfToken::Type::StreamEnd),
        t(PdfToken::Type::Keyword),
    };
    PDTK_ASSERT_EQ(toks.size(), sizeof(expected) / sizeof(expected[0]));
    for (std::size_t i = 0; i < toks.size(); ++i) {
        PDTK_ASSERT_EQ(t(toks[i].type), expected[i]);
    }
}

PDTK_TEST(whitespace_and_comments_skipped) {
    // NUL, TAB, LF, FF, CR, SP and a comment between every token.
    const auto doc = "\x00\x09\x0a\x0c\x0d\x20% a comment\r\n"
                     "Tj\x00% another\nTJ"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{2});
    PDTK_ASSERT(toks[0].value == "Tj");
    PDTK_ASSERT(toks[1].value == "TJ");
}

PDTK_TEST(literal_strings_escapes_and_nesting) {
    const auto doc = "(simple) (with \\) paren) ((nested (deep))) "
                     "(escape \\\\ \\<\\>)"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{4});
    PDTK_ASSERT(toks[0].value == "(simple)");
    PDTK_ASSERT(toks[1].value == "(with \\) paren)");
    PDTK_ASSERT(toks[2].value == "((nested (deep)))");
    PDTK_ASSERT(toks[3].value == "(escape \\\\ \\<\\>)");
}

PDTK_TEST(unterminated_string_ends_at_buffer_end) {
    const auto doc = "(no close"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{1});
    PDTK_ASSERT(toks[0].type == PdfToken::Type::StringLit);
    PDTK_ASSERT(toks[0].value == "(no close");
}

PDTK_TEST(hex_strings_and_dict_disambiguation) {
    const auto doc = "<A1B2C3> << /Hex <4C6F6E6779> >>"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{5});
    PDTK_ASSERT(toks[0].type == PdfToken::Type::HexStr);
    PDTK_ASSERT(toks[0].value == "<A1B2C3>");
    PDTK_ASSERT(toks[1].type == PdfToken::Type::DictStart);
    PDTK_ASSERT(toks[2].type == PdfToken::Type::Name);
    PDTK_ASSERT(toks[3].type == PdfToken::Type::HexStr);
    PDTK_ASSERT(toks[3].value == "<4C6F6E6779>");
    PDTK_ASSERT(toks[4].type == PdfToken::Type::DictEnd);
}

PDTK_TEST(names_keep_hash_escapes_raw) {
    // The token value is the RAW name: decoding needs a copy and is the
    // consumer's concern (zero-copy contract, header-documented).
    const auto doc = "/A#20B /Name1 / /plain"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{4});
    PDTK_ASSERT(toks[0].type == PdfToken::Type::Name);
    PDTK_ASSERT(toks[0].value == "/A#20B");
    PDTK_ASSERT(toks[1].value == "/Name1");
    PDTK_ASSERT(toks[2].value == "/");  // empty (bare) name
    PDTK_ASSERT(toks[3].value == "/plain");
}

PDTK_TEST(integers_parse_with_sign_and_saturate) {
    const auto doc = "42 +7 -13 0 2147483647 9223372036854775807 "
                     "99999999999999999999999 -99999999999999999999999"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{8});
    for (const PdfToken& tok : toks) {
        PDTK_ASSERT(tok.type == PdfToken::Type::Integer);
    }
    PDTK_ASSERT_EQ(toks[0].int_val, std::int64_t{42});
    PDTK_ASSERT_EQ(toks[1].int_val, std::int64_t{7});   // '+' accepted
    PDTK_ASSERT_EQ(toks[2].int_val, std::int64_t{-13});
    PDTK_ASSERT_EQ(toks[4].int_val, std::int64_t{2147483647});
    PDTK_ASSERT_EQ(toks[5].int_val, std::int64_t{INT64_MAX});
    PDTK_ASSERT_EQ(toks[6].int_val, std::int64_t{INT64_MAX});   // saturate
    PDTK_ASSERT_EQ(toks[7].int_val, std::int64_t{INT64_MIN});   // saturate
}

PDTK_TEST(reals_parse) {
    const auto doc = "1.5 -0.5 .5 3.14159 1e3 -2.5e-2 0.0"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{7});
    for (const PdfToken& tok : toks) {
        PDTK_ASSERT(tok.type == PdfToken::Type::Real);
    }
    PDTK_ASSERT(toks[0].float_val > 1.49F && toks[0].float_val < 1.51F);
    PDTK_ASSERT(toks[1].float_val < -0.49F && toks[1].float_val > -0.51F);
    PDTK_ASSERT(toks[2].float_val > 0.49F && toks[2].float_val < 0.51F);
    PDTK_ASSERT(toks[4].float_val > 999.0F && toks[4].float_val < 1001.0F);
    PDTK_ASSERT(toks[5].float_val < -0.024F &&
                toks[5].float_val > -0.026F);
}

PDTK_TEST(keywords_include_booleans_and_null) {
    const auto doc = "true false null cm Tj TJ Do BT ET"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{9});
    for (const PdfToken& tok : toks) {
        PDTK_ASSERT(tok.type == PdfToken::Type::Keyword);
    }
    PDTK_ASSERT(toks[0].value == "true");
    PDTK_ASSERT(toks[2].value == "null");
}

PDTK_TEST(stream_contract_offset_points_past_keyword) {
    const auto doc = "<< /Length 5 >>\nstream\nBIN\x01\x02\x03\x04\x05\n"
                     "endstream\n"_b;
    ZeroCopyLexer lexer(doc);
    PDTK_ASSERT(lexer.next().type == PdfToken::Type::DictStart);
    PDTK_ASSERT(lexer.next().type == PdfToken::Type::Name);      // /Length
    PDTK_ASSERT(lexer.next().type == PdfToken::Type::Integer);   // 5
    PDTK_ASSERT(lexer.next().type == PdfToken::Type::DictEnd);
    const PdfToken s = lexer.next();
    PDTK_ASSERT(s.type == PdfToken::Type::StreamStart);
    // The caller owns the payload: offset() is just past the keyword;
    // skip the single EOL to reach the data.
    const std::size_t data = lexer.offset() + 1;  // the '\n'
    PDTK_ASSERT_EQ(doc[data], std::uint8_t{'B'});
    PDTK_ASSERT(std::memcmp(&doc[data], "BIN\x01\x02\x03\x04\x05", 8) == 0);
    // The caller owns the payload: lexing RESUMES by constructing a
    // fresh lexer over the suffix at the endstream position (the
    // documented contract — the lexer never scans payload bytes).
    const std::string_view view(
        reinterpret_cast<const char*>(doc.data()), doc.size());
    const std::size_t es = view.find("endstream");
    PDTK_ASSERT(es != std::string_view::npos);
    ZeroCopyLexer resume(std::span<const std::uint8_t>(doc.data() + es,
                                                      doc.size() - es));
    PDTK_ASSERT(resume.next().type == PdfToken::Type::StreamEnd);
}

PDTK_TEST(stray_close_angle_and_braces_are_skipped) {
    // A stray '>' and the type-4 braces are skipped; a lone '<' is NOT
    // stray — it opens a hex string (covered by the hex tests).
    const auto doc = "a > b { c } d"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{4});
    for (const PdfToken& tok : toks) {
        PDTK_ASSERT(tok.type == PdfToken::Type::Keyword);
    }
    PDTK_ASSERT(toks[0].value == "a");
    PDTK_ASSERT(toks[1].value == "b");
    PDTK_ASSERT(toks[2].value == "c");
    PDTK_ASSERT(toks[3].value == "d");
}

PDTK_TEST(eof_is_idempotent) {
    const auto doc = "one two"_b;
    ZeroCopyLexer lexer(doc);
    (void)lexer.next();
    (void)lexer.next();
    for (int i = 0; i < 3; ++i) {
        const PdfToken tok = lexer.next();
        PDTK_ASSERT(tok.type == PdfToken::Type::EndOfFile);
        PDTK_ASSERT(tok.value.empty());
    }
}

PDTK_TEST(token_values_point_into_the_buffer) {
    const auto doc = "123 /Name (str) <hex> Tj"_b;
    const auto toks = lex_all(doc);
    const auto* base = doc.data();
    for (const PdfToken& tok : toks) {
        const auto* first =
            reinterpret_cast<const std::uint8_t*>(tok.value.data());
        PDTK_ASSERT(first >= base);
        PDTK_ASSERT(first + tok.value.size() <= base + doc.size());
    }
    PDTK_ASSERT(toks[0].value.data() == reinterpret_cast<const char*>(base));
}

PDTK_TEST(number_with_garbage_suffix_stays_tolerant) {
    const auto doc = "12abc 3..4"_b;
    const auto toks = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), std::size_t{2});
    PDTK_ASSERT(toks[0].type == PdfToken::Type::Integer);
    PDTK_ASSERT_EQ(toks[0].int_val, std::int64_t{12});
    PDTK_ASSERT(toks[0].value == "12abc");  // raw preserved
    PDTK_ASSERT(toks[1].type == PdfToken::Type::Real);  // "3..4"
    PDTK_ASSERT(toks[1].float_val > 2.99F && toks[1].float_val < 3.01F);
}

PDTK_TEST(junk_runs_do_not_recurse_or_hang) {
    // A megabyte of skip-class bytes: the iterative skip loop must
    // neither blow the stack nor stall (regression for the recursive
    // formulation caught in review).
    std::vector<std::uint8_t> doc(1'000'000, static_cast<std::uint8_t>('{'));
    doc.push_back('x');
    ZeroCopyLexer lexer(doc);
    const PdfToken tok = lexer.next();
    PDTK_ASSERT(tok.type == PdfToken::Type::Keyword);
    PDTK_ASSERT(tok.value == "x");
    PDTK_ASSERT(lexer.next().type == PdfToken::Type::EndOfFile);
}

PDTK_TEST(realistic_content_stream) {
    std::vector<std::uint8_t> doc;
    for (int i = 0; i < 50; ++i) {
        const std::string chunk =
            "BT /F1 12 Tf 1 0 0 1 72 720 Tm (Hello world #" +
            std::to_string(i) + ") Tj 0 -14 Td ET\nq 0.5 w 1 0 0 RG "
            "[] 0 d 72 700 m 300 700 l S Q\n";
        doc.insert(doc.end(), chunk.begin(), chunk.end());
    }
    const auto toks = lex_all(doc);
    PDTK_ASSERT(toks.size() > 50 * 30);
    // Deterministic re-lex: identical streams.
    const auto again = lex_all(doc);
    PDTK_ASSERT_EQ(toks.size(), again.size());
    for (std::size_t i = 0; i < toks.size(); ++i) {
        PDTK_ASSERT_EQ(t(toks[i].type), t(again[i].type));
        PDTK_ASSERT(toks[i].value == again[i].value);
        PDTK_ASSERT_EQ(toks[i].int_val, again[i].int_val);
    }
}

PDTK_TEST(hostile_buffers_lex_without_crashing) {
    // Deterministic random buffers (LCG): every byte value, including
    // NULs, high bytes and unterminated constructs. The lexer must
    // terminate and produce a deterministic stream (ASan/UBSan runs of
    // this suite are the memory-safety proof).
    std::uint32_t state = 12345;
    const auto next_byte = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>(state >> 13);
    };
    for (int round = 0; round < 200; ++round) {
        std::vector<std::uint8_t> doc;
        doc.resize(256 + (round % 2000));
        for (auto& b : doc) {
            b = next_byte();
        }
        const auto first = lex_all(doc);
        const auto second = lex_all(doc);
        PDTK_ASSERT_EQ(first.size(), second.size());
        for (std::size_t i = 0; i < first.size(); ++i) {
            PDTK_ASSERT_EQ(t(first[i].type), t(second[i].type));
            PDTK_ASSERT(first[i].value == second[i].value);
        }
    }
}

PDTK_TEST(empty_buffer_is_single_eof) {
    ZeroCopyLexer lexer(std::span<const std::uint8_t>{});
    PDTK_ASSERT(lexer.next().type == PdfToken::Type::EndOfFile);
    PDTK_ASSERT_EQ(lexer.offset(), std::size_t{0});
}

}  // namespace

PDTK_TEST_MAIN()
