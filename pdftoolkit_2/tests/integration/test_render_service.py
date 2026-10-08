"""Integration tests for the render service."""

import base64

import pytest

from pdftoolkit.core.errors import ValidationError
from pdftoolkit.documents import DocumentRegistry
from pdftoolkit.services import RenderService

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


@pytest.fixture
def service():
    return RenderService(DocumentRegistry())


class TestRenderPages:
    def test_renders_base64_png(self, service, sample_pdf):
        result = service.render_pages(sample_pdf, pages="1")
        assert result["pages_rendered"] == 1
        raw = base64.b64decode(result["images"]["1"])
        assert raw.startswith(PNG_MAGIC)

    def test_renders_all_pages_by_default(self, service, sample_pdf):
        result = service.render_pages(sample_pdf)
        assert result["pages_rendered"] == 5
        assert set(result["images"]) == {"1", "2", "3", "4", "5"}

    def test_dpi_reported(self, service, sample_pdf):
        assert service.render_pages(sample_pdf, dpi=72)["dpi"] == 72

    def test_higher_dpi_larger_payload(self, service, sample_pdf):
        small = service.render_pages(sample_pdf, pages="1", dpi=50)
        large = service.render_pages(sample_pdf, pages="1", dpi=200)
        assert len(large["images"]["1"]) > len(small["images"]["1"])

    @pytest.mark.parametrize("dpi", [0, -5, 2000, "high", None])
    def test_invalid_dpi_rejected(self, service, sample_pdf, dpi):
        with pytest.raises(ValidationError):
            service.render_pages(sample_pdf, dpi=dpi)

    def test_invalid_pages_rejected(self, service, sample_pdf):
        with pytest.raises(Exception):
            service.render_pages(sample_pdf, pages="99")

    def test_images_are_json_safe_strings(self, service, sample_pdf):
        result = service.render_pages(sample_pdf, pages="1")
        assert isinstance(result["images"]["1"], str)


class TestRenderToFiles:
    def test_writes_png_files(self, service, sample_pdf, tmp_path):
        out_dir = tmp_path / "renders"
        result = service.render_to_files(sample_pdf, output_dir=out_dir, pages="1-2")
        assert result["count"] == 2
        assert result["directory"] == str(out_dir)
        for path in result["files"]:
            assert path.endswith(".png")
            with open(path, "rb") as handle:
                assert handle.read(8) == PNG_MAGIC

    def test_default_directory_is_document_folder(self, service, sample_pdf):
        result = service.render_to_files(sample_pdf, pages="1")
        assert result["directory"] == str(sample_pdf.parent)
        assert len(result["files"]) == 1

    def test_filename_pattern(self, service, sample_pdf, tmp_path):
        out_dir = tmp_path / "renders"
        result = service.render_to_files(sample_pdf, output_dir=out_dir, pages="3")
        assert result["files"][0].endswith("sample-page-0003.png")

    def test_custom_prefix(self, service, sample_pdf, tmp_path):
        out_dir = tmp_path / "renders"
        result = service.render_to_files(
            sample_pdf, output_dir=out_dir, pages="1", prefix="invoice"
        )
        assert result["files"][0].endswith("invoice-page-0001.png")

    def test_never_overwrites_existing_files(self, service, sample_pdf, tmp_path):
        out_dir = tmp_path / "renders"
        first = service.render_to_files(sample_pdf, output_dir=out_dir, pages="1")
        second = service.render_to_files(sample_pdf, output_dir=out_dir, pages="1")
        assert first["files"] != second["files"]
        assert "(1)" in second["files"][0]

    def test_creates_missing_output_directory(self, service, sample_pdf, tmp_path):
        out_dir = tmp_path / "deep" / "nested" / "renders"
        result = service.render_to_files(sample_pdf, output_dir=out_dir, pages="1")
        assert out_dir.is_dir()
        assert result["count"] == 1
