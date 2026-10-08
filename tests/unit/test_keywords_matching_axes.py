"""Unit tests for the configurable match axes (case sensitivity, word
boundaries) that unify the original pdf-search and pdftoolkit behaviours."""

from pdftoolkit.core.keywords import (
    contains_keyword,
    count_keywords,
    first_match_snippet,
    keyword_pattern,
    keywords_with_matches,
)

TEXT = "The Quick Brown Fox jumps. quick alphabet and QUICKLY again."


class TestCaseSensitivity:
    def test_default_is_case_insensitive(self):
        # "Quick" and "quick" match; "QUICKLY" is a different word
        assert count_keywords(TEXT, ["quick"])["quick"] == 2

    def test_case_sensitive_matches_exact_case_only(self):
        assert count_keywords(TEXT, ["quick"], case_sensitive=True)["quick"] == 1

    def test_case_sensitive_uppercase_query(self):
        # "QUICKLY" is a different word, so nothing matches exactly
        assert count_keywords(TEXT, ["QUICK"], case_sensitive=True)["QUICK"] == 0

    def test_case_insensitive_uppercase_query(self):
        assert count_keywords(TEXT, ["QUICK"], case_sensitive=False)["QUICK"] == 2

    def test_contains_keyword_respects_case(self):
        assert contains_keyword(TEXT, "quick", case_sensitive=True) is True
        assert contains_keyword("only LOWER case", "lower", case_sensitive=True) is False
        assert contains_keyword("only LOWER case", "lower", case_sensitive=False) is True


class TestWholeWords:
    def test_default_uses_word_boundaries(self):
        # "alpha" must not match inside "alphabet"
        assert count_keywords(TEXT, ["alpha"])["alpha"] == 0

    def test_substring_mode_matches_prefixes(self):
        # The original pdf-search backend counted substrings.
        assert count_keywords(TEXT, ["alpha"], whole_words=False)["alpha"] == 1

    def test_substring_mode_matches_inside_words(self):
        text = "alphabet alphabetical"
        assert count_keywords(text, ["alpha"], whole_words=False)["alpha"] == 2
        assert count_keywords(text, ["alpha"], whole_words=True)["alpha"] == 0

    def test_word_boundaries_with_punctuation(self):
        assert count_keywords("run, run! (run)", ["run"])["run"] == 3

    def test_substring_respects_case_sensitivity_too(self):
        text = "Alpha alphabet ALPHA"
        assert count_keywords(text, ["alpha"], case_sensitive=True, whole_words=False)["alpha"] == 1
        assert count_keywords(text, ["alpha"], case_sensitive=False, whole_words=False)["alpha"] == 3
        assert count_keywords(text, ["Alpha"], case_sensitive=True, whole_words=False)["Alpha"] == 1


class TestKeywordPattern:
    def test_regex_metacharacters_are_escaped(self):
        pattern = keyword_pattern("a.b*c")
        assert pattern.search("a.b*c") is not None
        assert pattern.search("axbxc") is None

    def test_substring_pattern_has_no_boundaries(self):
        pattern = keyword_pattern("alpha", whole_words=False)
        assert pattern.search("alphabet") is not None

    def test_case_sensitive_pattern_compiles_without_flags(self):
        pattern = keyword_pattern("Quick", case_sensitive=True)
        assert pattern.search("quick") is None
        assert pattern.search("Quick") is not None


class TestSnippetAxes:
    def test_snippet_default_case_insensitive(self):
        assert first_match_snippet(TEXT, "fox") is not None

    def test_snippet_case_sensitive_miss_returns_none(self):
        assert first_match_snippet(TEXT, "FOX", case_sensitive=True) is None

    def test_snippet_substring_mode(self):
        assert first_match_snippet("alphabet", "alpha", whole_words=False) is not None
        assert first_match_snippet("alphabet", "alpha", whole_words=True) is None


class TestKeywordsWithMatches:
    def test_filters_positive_counts_in_order(self):
        assert keywords_with_matches({"alpha": 2, "beta": 0, "gamma": 1}) == ["alpha", "gamma"]

    def test_empty_when_no_matches(self):
        assert keywords_with_matches({"alpha": 0}) == []

