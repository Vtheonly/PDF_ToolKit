"""Unit tests for padding-based page expansion (the unified
``calculate_page_range`` of the original pdf-search backend)."""

import pytest

from pdftoolkit.core.errors import PageRangeError
from pdftoolkit.core.pages import expand_with_padding


class TestExpandWithPadding:
    def test_single_page_with_default_padding(self):
        assert expand_with_padding([5], 10, padding=2) == [3, 4, 5, 6, 7]

    def test_padding_zero_returns_pages_themselves(self):
        assert expand_with_padding([3, 7], 10, padding=0) == [3, 7]

    def test_windows_merge_when_overlapping(self):
        # pages 4 and 6 with padding 1: [3,4,5] + [5,6,7] -> merged
        assert expand_with_padding([4, 6], 10, padding=1) == [3, 4, 5, 6, 7]

    def test_disjoint_windows_stay_apart(self):
        assert expand_with_padding([2, 8], 10, padding=1) == [1, 2, 3, 7, 8, 9]

    def test_duplicates_in_input_collapse(self):
        assert expand_with_padding([4, 4, 4], 10, padding=1) == [3, 4, 5]

    def test_result_is_sorted_even_for_unsorted_input(self):
        assert expand_with_padding([8, 2], 10, padding=1) == [1, 2, 3, 7, 8, 9]

    def test_window_clamped_at_document_start(self):
        assert expand_with_padding([1], 5, padding=3) == [1, 2, 3, 4]

    def test_window_clamped_at_document_end(self):
        assert expand_with_padding([5], 5, padding=3) == [2, 3, 4, 5]

    def test_huge_padding_covers_whole_document(self):
        assert expand_with_padding([3], 5, padding=100) == [1, 2, 3, 4, 5]

    def test_empty_page_list_returns_empty(self):
        assert expand_with_padding([], 10, padding=2) == []

    def test_original_pdf_search_semantics(self):
        # The exact scenario from the original pdf-search demo:
        # matched pages [3, 5] in a 5-page document with padding 2.
        assert expand_with_padding([3, 5], 5, padding=2) == [1, 2, 3, 4, 5]

    def test_unsorted_matches_original_semantics(self):
        # matched pages [5, 3] must behave identically to [3, 5]
        assert expand_with_padding([5, 3], 5, padding=2) == [1, 2, 3, 4, 5]


class TestExpandWithPaddingValidation:
    def test_negative_padding_rejected(self):
        with pytest.raises(PageRangeError):
            expand_with_padding([1], 5, padding=-1)

    def test_boolean_padding_rejected(self):
        with pytest.raises(PageRangeError):
            expand_with_padding([1], 5, padding=True)

    def test_non_integer_padding_rejected(self):
        with pytest.raises(PageRangeError):
            expand_with_padding([1], 5, padding=1.5)

    def test_zero_page_document_rejected(self):
        with pytest.raises(PageRangeError):
            expand_with_padding([1], 0)

    def test_out_of_range_page_rejected(self):
        with pytest.raises(PageRangeError) as excinfo:
            expand_with_padding([6], 5)
        assert excinfo.value.details["page"] == 6
        assert excinfo.value.details["total_pages"] == 5

    def test_non_integer_page_rejected(self):
        with pytest.raises(PageRangeError):
            expand_with_padding(["3"], 5)

    def test_boolean_page_rejected(self):
        with pytest.raises(PageRangeError):
            expand_with_padding([True], 5)
