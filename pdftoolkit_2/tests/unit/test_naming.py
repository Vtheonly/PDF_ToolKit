"""Unit tests for natural ordering and output naming helpers."""

from pdftoolkit.utils.naming import (
    natural_key,
    natural_sorted,
    render_filename,
    stem,
)


class TestNaturalKey:
    def test_numbers_sort_numerically(self):
        assert natural_key("file2") < natural_key("file10")

    def test_lexicographic_trap_avoided(self):
        assert sorted(["10", "2", "1"], key=natural_key) == ["1", "2", "10"]

    def test_case_insensitive(self):
        assert natural_key("Beta") == natural_key("beta")

    def test_pure_numbers(self):
        assert natural_key("42") == [42]

    def test_mixed_chunks(self):
        key = natural_key("page-12-final")
        assert key == ["page-", 12, "-final"]


class TestNaturalSorted:
    def test_merge_folder_ordering(self):
        names = ["10_zeta.pdf", "2_beta.pdf", "1_alpha.pdf"]
        assert natural_sorted(names) == ["1_alpha.pdf", "2_beta.pdf", "10_zeta.pdf"]

    def test_key_function(self):
        items = [(3, "c"), (1, "a"), (2, "b")]
        result = natural_sorted(items, key=lambda pair: pair[1])
        assert result == [(1, "a"), (2, "b"), (3, "c") ]

    def test_stability_for_equal_keys(self):
        items = ["b", "a", "b"]
        assert natural_sorted(items) == ["a", "b", "b"]

    def test_empty_input(self):
        assert natural_sorted([]) == []

    def test_handles_none_like_via_key(self):
        class Named:
            def __init__(self, name):
                self.name = name

        result = natural_sorted([Named("10"), Named("2")], key=lambda item: item.name)
        assert [item.name for item in result] == ["2", "10"]


class TestStem:
    def test_removes_suffix(self):
        assert stem("reports/invoice.pdf") == "invoice"

    def test_multi_dot_filename(self):
        assert stem("archive.tar.gz") == "archive.tar"

    def test_no_suffix(self):
        assert stem("plain") == "plain"

    def test_pathlike(self, tmp_path):
        target = tmp_path / "doc.pdf"
        assert stem(target) == "doc"


class TestRenderFilename:
    def test_default_prefix(self):
        assert render_filename(None, 3) == "page-0003.png"

    def test_explicit_prefix(self):
        assert render_filename("invoice", 12) == "invoice-0012.png"

    def test_pages_are_zero_padded_for_ordering(self):
        assert render_filename(None, 7) < render_filename(None, 100)
