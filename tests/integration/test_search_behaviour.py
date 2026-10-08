"""Integration tests for the configurable search behaviour: match axes,
per-page match summaries, recursive corpus scans and relevance ranking."""

import pytest

from pdftoolkit.core.errors import ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import SearchService


@pytest.fixture
def service():
    return SearchService(DocumentRegistry())


@pytest.fixture
def cased_pdf(tmp_path):
    from helpers import make_pdf

    return make_pdf(
        tmp_path / "cased.pdf",
        [
            "The Quick brown fox",
            "quick quick QUICK",
            "alphabet soup",
            "nothing relevant",
        ],
    )


class TestMatchAxes:
    def test_default_is_case_insensitive_word_match(self, service, cased_pdf):
        result = service.search(cased_pdf, ["quick"])
        assert result["totals"] == {"quick": 4}
        assert result["case_sensitive"] is False
        assert result["whole_words"] is True

    def test_case_sensitive_counts_exact_case(self, service, cased_pdf):
        result = service.search(cased_pdf, ["quick"], case_sensitive=True)
        # only the two lowercase occurrences on page 2 match
        assert result["totals"] == {"quick": 2}

    def test_word_boundaries_exclude_embedded_matches(self, service, cased_pdf):
        result = service.search(cased_pdf, ["alpha"])
        assert result["totals"] == {"alpha": 0}

    def test_substring_mode_includes_embedded_matches(self, service, cased_pdf):
        result = service.search(cased_pdf, ["alpha"], whole_words=False)
        assert result["totals"] == {"alpha": 1}
        assert result["page_matches"][0]["page"] == 3

    def test_axes_combine(self, service, cased_pdf):
        result = service.search(
            cased_pdf, ["QUICK"], case_sensitive=True, whole_words=False
        )
        # page 2 contains one uppercase "QUICK" token
        assert result["totals"] == {"QUICK": 1}


class TestPageMatchSummaries:
    """The pdf-search MatchResult contract: page, count, keywords_found."""

    @pytest.fixture
    def summary_pdf(self, tmp_path):
        from helpers import make_pdf

        return make_pdf(
            tmp_path / "summary.pdf",
            ["alpha beta alpha", "gamma", "beta beta beta delta"],
        )

    def test_page_matches_list_keywords_per_page(self, service, summary_pdf):
        result = service.search(summary_pdf, ["alpha", "beta", "gamma"])
        by_page = {entry["page"]: entry for entry in result["page_matches"]}
        assert by_page[1] == {"page": 1, "count": 3, "keywords_found": ["alpha", "beta"]}
        assert by_page[2] == {"page": 2, "count": 1, "keywords_found": ["gamma"]}
        assert by_page[3] == {"page": 3, "count": 3, "keywords_found": ["beta"]}

    def test_pages_without_matches_are_absent(self, service, cased_pdf):
        result = service.search(cased_pdf, ["alpha"])
        # page 3 contains "alpha" only inside the word "alphabet"
        assert result["page_matches"] == []

    def test_matched_pages_is_page_number_list(self, service, summary_pdf):
        result = service.search(summary_pdf, ["gamma", "delta"])
        assert result["matched_pages"] == [2, 3]

    def test_matches_counts_keyword_hits(self, service, summary_pdf):
        result = service.search(summary_pdf, ["alpha", "beta", "gamma"])
        # hits are per (page, keyword) pairs with a positive count
        assert result["matches"] == 4

    def test_case_sensitive_summary(self, service, cased_pdf):
        result = service.search(cased_pdf, ["quick"], case_sensitive=True)
        assert result["matched_pages"] == [2]

    def test_snippets_present_on_hits(self, service, summary_pdf):
        result = service.search(summary_pdf, ["gamma"])
        assert result["hits"][0]["snippet"] is not None


class TestCorpusRecursiveScan:
    @pytest.fixture
    def tree(self, tmp_path):
        from helpers import make_pdf, make_corrupt_pdf

        root = tmp_path / "tree"
        (root / "sub" / "deep").mkdir(parents=True)
        make_pdf(root / "top.pdf", ["alpha"])
        make_pdf(root / "sub" / "nested.pdf", ["alpha alpha"])
        make_pdf(root / "sub" / "deep" / "deepest.pdf", ["alpha alpha alpha"])
        make_pdf(root / "unrelated.txt", ["alpha"])
        make_corrupt_pdf(root / "broken.pdf")
        return root

    def test_non_recursive_scans_top_level_only(self, service, tree):
        result = service.search_corpus(["alpha"], directory=tree)
        assert result["documents_scanned"] == 1
        assert result["recursive"] is False

    def test_recursive_scans_subfolders(self, service, tree):
        result = service.search_corpus(["alpha"], directory=tree, recursive=True)
        assert result["documents_scanned"] == 3
        assert result["recursive"] is True

    def test_unreadable_pdfs_are_skipped_not_fatal(self, service, tree):
        result = service.search_corpus(["alpha"], directory=tree, recursive=True)
        assert {entry["reason"] for entry in result["skipped"]} == {"unreadable_pdf"}
        assert result["documents_scanned"] == 3

    def test_recursive_matches_original_glob_semantics(self, service, tree):
        """The original web engines globbed ``directory/**/*.pdf``."""
        result = service.search_corpus(["alpha"], directory=tree, recursive=True)
        names = sorted(doc["document"]["filename"] for doc in result["documents"])
        assert names == ["deepest.pdf", "nested.pdf", "top.pdf"]


class TestCorpusRanking:
    @pytest.fixture
    def rank_tree(self, tmp_path):
        from helpers import make_pdf

        root = tmp_path / "rank"
        root.mkdir()
        make_pdf(root / "a_few.pdf", ["alpha"])
        make_pdf(root / "b_many.pdf", ["alpha alpha alpha"])
        make_pdf(root / "c_none.pdf", ["nothing"])
        make_pdf(root / "d_some.pdf", ["alpha alpha"])
        return root

    def test_path_sort_is_default_and_natural(self, service, rank_tree):
        result = service.search_corpus(["alpha"], directory=rank_tree)
        assert result["sort"] == "path"
        names = [doc["document"]["filename"] for doc in result["documents"]]
        assert names == ["a_few.pdf", "b_many.pdf", "c_none.pdf", "d_some.pdf"]
        assert all("rank" not in doc for doc in result["documents"])

    def test_rank_sort_orders_by_total_occurrences(self, service, rank_tree):
        result = service.search_corpus(["alpha"], directory=rank_tree, sort="rank")
        names = [doc["document"]["filename"] for doc in result["documents"]]
        assert names == ["b_many.pdf", "d_some.pdf", "a_few.pdf", "c_none.pdf"]

    def test_rank_sort_assigns_ranks(self, service, rank_tree):
        result = service.search_corpus(["alpha"], directory=rank_tree, sort="rank")
        ranks = [doc["rank"] for doc in result["documents"]]
        assert ranks == [1, 2, 3, 4]

    def test_rank_ties_break_by_natural_path(self, service, tmp_path):
        from helpers import make_pdf

        root = tmp_path / "ties"
        root.mkdir()
        make_pdf(root / "10_late.pdf", ["alpha"])
        make_pdf(root / "2_early.pdf", ["alpha"])
        result = service.search_corpus(["alpha"], directory=root, sort="rank")
        names = [doc["document"]["filename"] for doc in result["documents"]]
        assert names == ["2_early.pdf", "10_late.pdf"]

    def test_ranking_reports_per_document_totals(self, service, rank_tree):
        result = service.search_corpus(["alpha"], directory=rank_tree, sort="rank")
        totals = {doc["document"]["filename"]: doc["total_matches"] for doc in result["documents"]}
        assert totals == {"b_many.pdf": 3, "d_some.pdf": 2, "a_few.pdf": 1, "c_none.pdf": 0}

    def test_corpus_respects_match_axes(self, service, rank_tree):
        result = service.search_corpus(
            ["alpha"], directory=rank_tree, sort="rank", whole_words=False
        )
        assert result["whole_words"] is False
        assert result["totals"] == {"alpha": 6}

    def test_invalid_sort_mode_rejected(self, service, rank_tree):
        with pytest.raises(ValidationError):
            service.search_corpus(["alpha"], directory=rank_tree, sort="relevance")

    def test_empty_corpus_returns_zero_structure(self, service, tmp_path):
        empty = tmp_path / "empty"
        empty.mkdir()
        result = service.search_corpus(["alpha"], directory=empty, sort="rank")
        assert result["documents_scanned"] == 0
        assert result["documents"] == []
        assert result["totals"] == {"alpha": 0}
