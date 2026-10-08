"""End-to-end tests reproducing the original applications' workflows on the
unified engine: the pdf-search upload/search/export pipeline, the pdfgrep
corpus ranking, the pdfm numeric-prefix merge and the pdfContextCutter
multi-range extraction."""

import pytest

from helpers import make_pdf, pdf_page_count, pdf_page_texts
from pdftoolkit import Toolkit


@pytest.fixture
def engine(speech_provider):
    return Toolkit(speech_provider=speech_provider)


class TestPdfSearchWorkflow:
    """The original pdf-search backend workflow: search a document, expand
    matches by a padding window, render previews, export the selection."""

    @pytest.fixture
    def document(self, tmp_path):
        return make_pdf(
            tmp_path / "manual.pdf",
            [
                "cover page",
                "installation chapter",
                "the widget must be calibrated",
                "troubleshooting notes",
                "widget errors explained",
                "glossary",
            ],
        )

    def test_search_reports_match_summary(self, engine, document):
        result = engine.search(document, ["widget", "glossary"])
        data = result["data"]
        assert data["page_matches"] == [
            {"page": 3, "count": 1, "keywords_found": ["widget"]},
            {"page": 5, "count": 1, "keywords_found": ["widget"]},
            {"page": 6, "count": 1, "keywords_found": ["glossary"]},
        ]
        assert data["matched_pages"] == [3, 5, 6]
        assert data["totals"] == {"widget": 2, "glossary": 1}

    def test_padding_expansion_matches_original_semantics(self, engine, document):
        result = engine.search(document, ["widget"])
        matched = result["data"]["matched_pages"]
        from pdftoolkit.core.pages import expand_with_padding

        padded = expand_with_padding(matched, total_pages=6, padding=2)
        assert padded == [1, 2, 3, 4, 5, 6]

    def test_previews_of_padded_selection(self, engine, document):
        result = engine.render_thumbnails(document, pages="3-5", scale=0.3)
        thumbs = result["data"]["thumbnails"]
        assert set(thumbs) == {"3", "4", "5"}
        assert all(t.startswith("data:image/png;base64,") for t in thumbs.values())

    def test_one_shot_export_replaces_manual_pipeline(self, engine, document, tmp_path):
        out = tmp_path / "filtered_output.pdf"
        result = engine.extract_matches(document, ["widget"], out, padding=2)
        assert result["ok"] is True
        assert result["data"]["pages_with_padding"] == [1, 2, 3, 4, 5, 6]
        assert pdf_page_count(out) == 6

    def test_tight_export_without_padding(self, engine, document, tmp_path):
        out = tmp_path / "tight.pdf"
        result = engine.extract_matches(document, ["widget"], out, padding=0)
        texts = pdf_page_texts(out)
        assert len(texts) == 2
        assert "widget" in texts[0]
        assert "widget" in texts[1]

    def test_no_match_export_is_structured_failure(self, engine, document, tmp_path):
        result = engine.extract_matches(document, ["nonexistent"], tmp_path / "x.pdf")
        assert result["ok"] is False
        assert result["error"]["code"] == "content_not_found"


class TestPdfgrepCorpusWorkflow:
    """The original searchPDFs.py workflow: scan a folder recursively for
    keywords and rank files by total occurrences."""

    @pytest.fixture
    def library(self, tmp_path):
        root = tmp_path / "library"
        (root / "parsers").mkdir(parents=True)
        make_pdf(root / "1_overview.pdf", ["flex and lex intro"])
        make_pdf(root / "parsers" / "2_lex.pdf", ["lex lex lex lexer details"])
        make_pdf(root / "parsers" / "3_yacc.pdf", ["yacc grammar yacc rules"])
        make_pdf(root / "4_misc.pdf", ["unrelated content"])
        return root

    def test_ranked_corpus_scan(self, engine, library):
        result = engine.search_corpus(["lex", "yacc"], directory=library, recursive=True, sort="rank")
        data = result["data"]
        assert data["documents_scanned"] == 4
        ranked = [(doc["document"]["filename"], doc["total_matches"]) for doc in data["documents"]]
        # 2_lex.pdf has the most "lex" hits; 3_yacc.pdf the yacc hits;
        # ties and zeros keep natural order afterwards.
        assert ranked[0][0] == "2_lex.pdf"
        assert ranked[0][1] == 3
        assert data["totals"] == {"lex": 4, "yacc": 2}

    def test_unreadable_files_skipped_not_fatal(self, engine, library):
        from helpers import make_corrupt_pdf

        make_corrupt_pdf(library / "broken.pdf")
        result = engine.search_corpus(["lex"], directory=library, recursive=True)
        assert result["ok"] is True
        assert len(result["data"]["skipped"]) == 1

    def test_non_recursive_matches_top_level_only(self, engine, library):
        result = engine.search_corpus(["yacc"], directory=library)
        assert result["data"]["documents_scanned"] == 2  # top-level PDFs only


class TestPdfmMergeWorkflow:
    """The original pdfm workflow: merge ``N_name.pdf`` chapters in order."""

    @pytest.fixture
    def chapters(self, tmp_path):
        root = tmp_path / "book"
        root.mkdir()
        make_pdf(root / "1_intro.pdf", ["intro 1", "intro 2"])
        make_pdf(root / "2_content.pdf", ["content 1"])
        make_pdf(root / "10_appendix.pdf", ["appendix 1", "appendix 2", "appendix 3"])
        make_pdf(root / "frontmatter.pdf", ["no numeric prefix"])
        return root

    def test_numeric_prefix_merge(self, engine, chapters):
        result = engine.merge_folder(chapters, numeric_prefix=True)
        assert result["ok"] is True
        texts = pdf_page_texts(chapters / "merged.pdf")
        assert len(texts) == 6
        assert "intro 1" in texts[0]
        assert "content 1" in texts[2]
        assert "appendix 3" in texts[5]

    def test_plain_merge_includes_everything(self, engine, chapters):
        result = engine.merge_folder(chapters)
        assert result["data"]["files"] == 4


class TestPdfContextCutterWorkflow:
    """The original pdfContextCutter workflow: build a new PDF from an
    explicit multi-range extraction plan."""

    @pytest.fixture
    def book(self, tmp_path):
        return make_pdf(tmp_path / "uml_book.pdf", [f"book page {i}" for i in range(1, 31)])

    def test_extraction_plan(self, engine, book, tmp_path):
        out = tmp_path / "class_diagram_bible.pdf"
        plan = "5-8,15-17,25-30"  # disjoint inclusive ranges across the book
        result = engine.cut_pages(book, plan, out)
        assert result["ok"] is True
        assert result["data"]["pages"] == 4 + 3 + 6
        texts = pdf_page_texts(out)
        assert texts[0].strip() == "book page 5"
        assert texts[4].strip() == "book page 15"
        assert texts[-1].strip() == "book page 30"

    def test_plan_with_single_pages(self, engine, book, tmp_path):
        result = engine.cut_pages(book, "1,30", tmp_path / "ends.pdf")
        texts = pdf_page_texts(tmp_path / "ends.pdf")
        assert texts[0].strip() == "book page 1"
        assert texts[1].strip() == "book page 30"


class TestUnifiedWorkflows:
    """Cross-capability workflows only possible in the unified engine."""

    def test_search_then_speak_found_text(self, engine, tmp_path, speech_provider):
        pdf = make_pdf(tmp_path / "notice.pdf", ["the invoice total is due"])
        found = engine.search(pdf, ["invoice"])
        snippet_text = found["data"]["hits"][0]["snippet"]
        spoken = engine.synthesize_speech(
            f"Attention: {snippet_text}", speak_aloud=True, rate=180, volume=0.9
        )
        assert spoken["ok"] is True
        assert speech_provider.spoken[0][1:] == (180.0, 0.9)

    def test_merge_then_search_corpus(self, engine, tmp_path):
        make_pdf(tmp_path / "1_alpha.pdf", ["alpha one"])
        make_pdf(tmp_path / "2_beta.pdf", ["beta one"])
        merged = engine.merge_folder(tmp_path)
        assert merged["ok"] is True
        result = engine.search_corpus(["alpha", "beta"], directory=tmp_path, sort="rank")
        # merged.pdf contains both keywords and ranks first
        assert result["data"]["documents"][0]["document"]["filename"] == "merged.pdf"

    def test_register_and_reuse_document_ids(self, engine, sample_pdf):
        first = engine.register(sample_pdf)
        doc_id = first["data"]["document"]["id"]
        again = engine.document_info(doc_id)
        assert again["data"]["document"]["id"] == doc_id
        result = engine.search(doc_id, ["alpha"])
        assert result["data"]["totals"] == {"alpha": 3}
