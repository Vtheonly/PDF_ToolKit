"""Integration tests for the search service (single documents and corpora)."""

import pytest

from helpers import make_pdf, write_text_file
from pdftoolkit.core.errors import DocumentNotFoundError, ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import SearchService


@pytest.fixture
def service():
    return SearchService(DocumentRegistry())


class TestSearchSingleDocument:
    def test_totals_across_pages(self, service, sample_pdf):
        result = service.search(sample_pdf, ["alpha"])
        assert result["totals"] == {"alpha": 3}
        assert result["pages_scanned"] == 5

    def test_hit_structure(self, service, sample_pdf):
        result = service.search(sample_pdf, ["alpha"])
        hits = result["hits"]
        assert [hit["page"] for hit in hits] == [1, 3]
        assert hits[0]["count"] == 1
        assert hits[1]["count"] == 2
        assert all("snippet" in hit for hit in hits)
        assert all(hit["keyword"] == "alpha" for hit in hits)

    def test_multiple_keywords(self, service, sample_pdf):
        result = service.search(sample_pdf, ["alpha", "beta", "gamma"])
        assert result["totals"] == {"alpha": 3, "beta": 2, "gamma": 2}

    def test_case_insensitive(self, service, sample_pdf):
        assert service.search(sample_pdf, ["ALPHA"])["totals"] == {"ALPHA": 3}

    def test_word_boundaries(self, service, tmp_path):
        path = make_pdf(tmp_path / "words.pdf", ["alphabet alphabetic"])
        assert service.search(path, ["alpha"])["totals"] == {"alpha": 0}

    def test_page_filter(self, service, sample_pdf):
        result = service.search(sample_pdf, ["alpha"], pages="1-2")
        assert result["totals"] == {"alpha": 1}
        assert result["pages_scanned"] == 2

    def test_single_page_spec(self, service, sample_pdf):
        result = service.search(sample_pdf, ["beta"], pages="5")
        assert result["totals"] == {"beta": 1}

    def test_matches_counts_hits(self, service, sample_pdf):
        result = service.search(sample_pdf, ["alpha", "beta"])
        # alpha hits pages 1 and 3; beta hits pages 1 and 5
        assert result["matches"] == 4
        assert len(result["hits"]) == 4

    def test_document_block_in_result(self, service, sample_pdf):
        result = service.search(sample_pdf, ["alpha"])
        assert result["document"]["filename"] == "sample.pdf"
        assert result["document"]["page_count"] == 5

    def test_keyword_deduplication(self, service, sample_pdf):
        result = service.search(sample_pdf, ["alpha", "alpha"])
        assert result["keywords"] == ["alpha"]

    def test_invalid_page_spec(self, service, sample_pdf):
        with pytest.raises(Exception):
            service.search(sample_pdf, ["alpha"], pages="99")

    def test_empty_keywords_rejected(self, service, sample_pdf):
        with pytest.raises(ValidationError):
            service.search(sample_pdf, [])

    def test_blank_keyword_rejected(self, service, sample_pdf):
        with pytest.raises(ValidationError):
            service.search(sample_pdf, ["   "])

    def test_unknown_document(self, service, tmp_path):
        with pytest.raises(DocumentNotFoundError):
            service.search(tmp_path / "ghost.pdf", ["alpha"])

    def test_corrupt_document(self, service, corrupt_pdf):
        with pytest.raises(Exception):
            service.search(corrupt_pdf, ["alpha"])


class TestSearchCorpus:
    def test_directory_mode(self, service, corpus_dir):
        result = service.search_corpus(["alpha"], directory=corpus_dir)
        # lex_one has 1 alpha; lex_three has 1 alpha
        assert result["totals"] == {"alpha": 2}
        assert result["documents_scanned"] == 2

    def test_directory_mode_aggregates_all_keywords(self, service, corpus_dir):
        result = service.search_corpus(["alpha", "beta", "gamma"], directory=corpus_dir)
        assert result["totals"] == {"alpha": 2, "beta": 1, "gamma": 1}

    def test_broken_pdfs_skipped_not_fatal(self, service, corpus_dir):
        result = service.search_corpus(["alpha"], directory=corpus_dir)
        assert result["skipped"] == [
            {"path": str(corpus_dir / "broken.pdf"), "reason": "unreadable_pdf"}
        ]

    def test_text_files_ignored(self, service, corpus_dir):
        result = service.search_corpus(["alpha"], directory=corpus_dir)
        filenames = [d["document"]["filename"] for d in result["documents"]]
        assert "unrelated.txt" not in filenames

    def test_registered_documents_mode(self, service, tmp_path):
        service.registry.register(make_pdf(tmp_path / "a.pdf", ["alpha alpha"]))
        service.registry.register(make_pdf(tmp_path / "b.pdf", ["beta"]))
        result = service.search_corpus(["alpha", "beta"])
        assert result["totals"] == {"alpha": 2, "beta": 1}
        assert result["documents_scanned"] == 2

    def test_empty_corpus(self, service, tmp_path):
        empty = tmp_path / "empty"
        empty.mkdir()
        result = service.search_corpus(["alpha"], directory=empty)
        assert result["documents_scanned"] == 0
        assert result["totals"] == {"alpha": 0}
        assert result["matches"] == 0

    def test_matches_summed_across_documents(self, service, corpus_dir):
        result = service.search_corpus(["alpha"], directory=corpus_dir)
        assert result["matches"] == 2
        assert result["documents"][0]["matches"] + result["documents"][1]["matches"] == 2
