"""Unit tests for the strict page-specification parser."""

import pytest

from pdftoolkit.core.errors import PageRangeError, ValidationError
from pdftoolkit.core.pages import (
    context_window,
    format_pages,
    parse_page_spec,
)


class TestParsePageSpecAll:
    def test_none_returns_all_pages(self):
        assert parse_page_spec(None, 5) == [1, 2, 3, 4, 5]

    def test_empty_string_returns_all_pages(self):
        assert parse_page_spec("", 3) == [1, 2, 3]

    def test_zero_returns_all_pages(self):
        assert parse_page_spec("0", 4) == [1, 2, 3, 4]

    def test_all_literal_is_case_insensitive(self):
        assert parse_page_spec("ALL", 2) == [1, 2]
        assert parse_page_spec("All", 2) == [1, 2]

    def test_star_returns_all_pages(self):
        assert parse_page_spec("*", 2) == [1, 2]

    def test_none_literal_returns_no_pages(self):
        assert parse_page_spec("none", 3) == []

    def test_zero_pages_document_is_rejected(self):
        with pytest.raises(PageRangeError):
            parse_page_spec(None, 0)


class TestParsePageSpecSelections:
    def test_single_page(self):
        assert parse_page_spec("3", 5) == [3]

    def test_int_spec(self):
        assert parse_page_spec(2, 5) == [2]

    def test_inclusive_range(self):
        assert parse_page_spec("2-4", 5) == [2, 3, 4]

    def test_range_equal_bounds(self):
        assert parse_page_spec("3-3", 5) == [3]

    def test_combined_tokens(self):
        assert parse_page_spec("1,3,5-7", 10) == [1, 3, 5, 6, 7]

    def test_whitespace_tolerated(self):
        assert parse_page_spec(" 1 , 3 - 4 ", 5) == [1, 3, 4]

    def test_duplicates_removed(self):
        assert parse_page_spec("1,1,1", 5) == [1]

    def test_result_is_sorted_even_if_input_is_not(self):
        assert parse_page_spec("4,2", 5) == [2, 4]

    def test_overlapping_tokens_merge(self):
        assert parse_page_spec("1-3,2-4", 5) == [1, 2, 3, 4]

    def test_iterable_of_ints(self):
        assert parse_page_spec([5, 1], 5) == [1, 5]


class TestParsePageSpecErrors:
    @pytest.mark.parametrize("spec", ["-1", "6", "99"])
    def test_out_of_range_pages(self, spec):
        with pytest.raises(PageRangeError):
            parse_page_spec(spec, 5)

    def test_reversed_range(self):
        with pytest.raises(PageRangeError):
            parse_page_spec("5-1", 5)

    def test_empty_token(self):
        with pytest.raises(PageRangeError):
            parse_page_spec("1,,3", 5)

    def test_trailing_comma(self):
        with pytest.raises(PageRangeError):
            parse_page_spec("1-3,", 5)

    def test_non_numeric_token(self):
        with pytest.raises(PageRangeError):
            parse_page_spec("abc", 5)

    def test_mixed_garbage(self):
        with pytest.raises(PageRangeError):
            parse_page_spec("1..3", 5)

    def test_negative_page_in_iterable(self):
        with pytest.raises(PageRangeError):
            parse_page_spec([1, -2], 5)

    def test_bool_is_not_a_page(self):
        with pytest.raises(PageRangeError):
            parse_page_spec(True, 5)

    def test_float_pages_rejected(self):
        with pytest.raises(PageRangeError):
            parse_page_spec([1.5], 5)

    def test_zero_start_of_range(self):
        with pytest.raises(PageRangeError):
            parse_page_spec("0-3", 5)

    def test_error_details_carry_context(self):
        with pytest.raises(PageRangeError) as excinfo:
            parse_page_spec("42", 5)
        assert excinfo.value.details["page"] == 42
        assert excinfo.value.details["total_pages"] == 5


class TestFormatPages:
    def test_empty(self):
        assert format_pages([]) == ""

    def test_single(self):
        assert format_pages([7]) == "7"

    def test_consecutive_run(self):
        assert format_pages([1, 2, 3, 4]) == "1-4"

    def test_runs_and_singles(self):
        assert format_pages([1, 2, 3, 5, 7, 8]) == "1-3,5,7-8"

    def test_unsorted_input_is_sorted(self):
        assert format_pages([5, 1, 2]) == "1-2,5"

    def test_duplicates_ignored(self):
        assert format_pages([2, 2, 1]) == "1-2"

    def test_roundtrip_with_parser(self):
        pages = [1, 2, 3, 6, 9, 10]
        assert parse_page_spec(format_pages(pages), 20) == pages


class TestContextWindow:
    def test_window_around_middle_page(self):
        assert context_window(3, 7, before=1, after=1) == [2, 3, 4]

    def test_window_clamped_at_start(self):
        assert context_window(1, 7, before=2, after=1) == [1, 2]

    def test_window_clamped_at_end(self):
        assert context_window(7, 7, before=1, after=3) == [6, 7]

    def test_zero_context(self):
        assert context_window(4, 7, before=0, after=0) == [4]

    def test_wide_window_covers_document(self):
        assert context_window(3, 4, before=10, after=10) == [1, 2, 3, 4]

    def test_anchor_always_included(self):
        assert 5 in context_window(5, 9, before=0, after=0)

    @pytest.mark.parametrize("page", [0, -1, 10])
    def test_anchor_out_of_range(self, page):
        with pytest.raises(PageRangeError):
            context_window(page, 9)

    @pytest.mark.parametrize("before,after", [(-1, 0), (0, -2)])
    def test_negative_context_rejected(self, before, after):
        with pytest.raises(PageRangeError):
            context_window(3, 9, before=before, after=after)

    def test_non_int_anchor_rejected(self):
        with pytest.raises(PageRangeError):
            context_window("3", 9)


class TestPageRangeErrorHierarchy:
    def test_is_a_validation_error(self):
        assert issubclass(PageRangeError, ValidationError)

    def test_carries_stable_code(self):
        assert PageRangeError("bad").code == "invalid_page_range"
