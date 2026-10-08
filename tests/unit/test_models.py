"""Unit tests for domain models."""

import pytest

from pdftoolkit.core.models import DocumentMeta, LinkInfo, MergeSource, SearchHit


class TestDocumentMeta:
    def test_fields(self):
        meta = DocumentMeta(id="doc-1", path="/tmp/x.pdf", filename="x.pdf", page_count=3)
        assert meta.id == "doc-1"
        assert meta.filename == "x.pdf"
        assert meta.page_count == 3

    def test_to_dict_shape(self):
        meta = DocumentMeta(id="doc-2", path="/tmp/y.pdf", filename="y.pdf", page_count=7)
        assert meta.to_dict() == {
            "id": "doc-2",
            "path": "/tmp/y.pdf",
            "filename": "y.pdf",
            "page_count": 7,
        }

    def test_page_count_optional(self):
        meta = DocumentMeta(id="doc-3", path="/tmp/z.pdf", filename="z.pdf")
        assert meta.page_count is None
        assert meta.to_dict()["page_count"] is None

    def test_frozen(self):
        meta = DocumentMeta(id="doc-1", path="/tmp/x.pdf", filename="x.pdf")
        with pytest.raises(Exception):
            meta.id = "doc-9"


class TestSearchHit:
    def test_to_dict_shape(self):
        hit = SearchHit(page=3, keyword="alpha", count=2, snippet="... alpha ...")
        assert hit.to_dict() == {
            "page": 3,
            "keyword": "alpha",
            "count": 2,
            "snippet": "... alpha ...",
        }

    def test_snippet_defaults_to_none(self):
        hit = SearchHit(page=1, keyword="x", count=1)
        assert hit.to_dict()["snippet"] is None


class TestLinkInfo:
    def test_to_dict_shape(self):
        link = LinkInfo(page=2, uri="https://example.com", rect=(1.0, 2.0, 3.0, 4.0))
        assert link.to_dict() == {
            "page": 2,
            "uri": "https://example.com",
            "rect": [1.0, 2.0, 3.0, 4.0],
        }

    def test_rect_defaults_to_zero(self):
        link = LinkInfo(page=1, uri="u")
        assert link.to_dict()["rect"] == [0.0, 0.0, 0.0, 0.0]

    def test_frozen(self):
        link = LinkInfo(page=1, uri="u")
        with pytest.raises(Exception):
            link.uri = "v"


class TestMergeSource:
    def test_to_dict_shape(self):
        source = MergeSource(path="/a.pdf", filename="a.pdf", pages=4)
        assert source.to_dict() == {"path": "/a.pdf", "filename": "a.pdf", "pages": 4}

    def test_pages_defaults_to_zero(self):
        assert MergeSource(path="/a.pdf", filename="a.pdf").to_dict()["pages"] == 0


class TestJsonSafety:
    """Model dicts must be JSON-serialisable without custom encoders."""

    def test_all_models_serialize(self):
        import json

        payload = {
            "document": DocumentMeta("doc-1", "/x.pdf", "x.pdf", 2).to_dict(),
            "hit": SearchHit(1, "k", 1, "s").to_dict(),
            "link": LinkInfo(1, "https://u", (0, 0, 1, 1)).to_dict(),
            "source": MergeSource("/x.pdf", "x.pdf", 2).to_dict(),
        }
        assert json.loads(json.dumps(payload)) == payload
