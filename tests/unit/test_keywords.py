"""Unit tests for keyword matching and aggregation primitives."""

import pytest

from pdftoolkit.core.errors import ValidationError
from pdftoolkit.core.keywords import (
    aggregate_totals,
    contains_keyword,
    count_keywords,
    first_match_snippet,
    normalize_keywords,
)


class TestNormalizeKeywords:
    def test_preserves_order_and_deduplicates(self):
        assert normalize_keywords(["alpha", "beta", "alpha"]) == ["alpha", "beta"]

    def test_single_string_is_wrapped(self):
        assert normalize_keywords("alpha") == ["alpha"]

    def test_strips_whitespace(self):
        assert normalize_keywords(["  alpha  "]) == ["alpha"]

    def test_empty_list_rejected(self):
        with pytest.raises(ValidationError):
            normalize_keywords([])

    def test_none_rejected(self):
        with pytest.raises(ValidationError):
            normalize_keywords(None)

    def test_empty_string_rejected(self):
        with pytest.raises(ValidationError):
            normalize_keywords([""])

    def test_whitespace_only_rejected(self):
        with pytest.raises(ValidationError):
            normalize_keywords(["   "])

    def test_non_iterable_rejected(self):
        with pytest.raises(ValidationError):
            normalize_keywords(42)


class TestCountKeywords:
    def test_counts_multiple_occurrences(self):
        assert count_keywords("alpha beta alpha", ["alpha"]) == {"alpha": 2}

    def test_case_insensitive(self):
        assert count_keywords("ALPHA alpha Alpha", ["alpha"]) == {"alpha": 3}

    def test_word_boundary_prevents_substring_match(self):
        assert count_keywords("alphabet", ["alpha"]) == {"alpha": 0}

    def test_multiple_keywords(self):
        counts = count_keywords("alpha beta alpha beta beta", ["alpha", "beta"])
        assert counts == {"alpha": 2, "beta": 3}

    def test_no_match_returns_zero(self):
        assert count_keywords("nothing here", ["alpha"]) == {"alpha": 0}

    def test_empty_text(self):
        assert count_keywords("", ["alpha"]) == {"alpha": 0}

    def test_none_text_treated_as_empty(self):
        assert count_keywords(None, ["alpha"]) == {"alpha": 0}

    def test_regex_special_characters_escaped(self):
        assert count_keywords("a.b axb", ["a.b"]) == {"a.b": 1}

    def test_multiline_text(self):
        assert count_keywords("alpha\nbeta\nalpha", ["alpha"]) == {"alpha": 2}

    def test_unicode_keyword(self):
        assert count_keywords("café café", ["café"]) == {"café": 2}


class TestContainsKeyword:
    def test_found(self):
        assert contains_keyword("the alpha wolf", "alpha") is True

    def test_case_insensitive_found(self):
        assert contains_keyword("the ALPHA wolf", "alpha") is True

    def test_not_found(self):
        assert contains_keyword("the wolf", "alpha") is False

    def test_substring_not_enough(self):
        assert contains_keyword("alphabet", "alpha") is False

    def test_empty_text(self):
        assert contains_keyword("", "alpha") is False


class TestFirstMatchSnippet:
    def test_returns_excerpt_around_match(self):
        snippet = first_match_snippet("lorem ipsum alpha dolor sit amet", "alpha")
        assert snippet is not None
        assert "alpha" in snippet

    def test_no_match_returns_none(self):
        assert first_match_snippet("no match here", "alpha") is None

    def test_short_text_has_no_ellipsis(self):
        snippet = first_match_snippet("alpha", "alpha")
        assert snippet == "alpha"

    def test_long_text_is_truncated_with_ellipsis(self):
        text = "x" * 200 + " alpha " + "y" * 200
        snippet = first_match_snippet(text, "alpha", width=10)
        assert snippet.startswith("...")
        assert snippet.endswith("...")


class TestAggregateTotals:
    def test_sums_across_entries(self):
        totals = aggregate_totals([{"a": 1, "b": 2}, {"a": 3}])
        assert totals == {"a": 4, "b": 2}

    def test_empty_input(self):
        assert aggregate_totals([]) == {}

    def test_missing_keys_default_to_zero(self):
        assert aggregate_totals([{"a": 1}, {"b": 5}]) == {"a": 1, "b": 5}

    def test_zero_counts_preserved(self):
        assert aggregate_totals([{"a": 0}]) == {"a": 0}
