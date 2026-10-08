"""IO layer: the isolated PDF backend and low-level file access."""

from .pdfio import PdfDocument, merge_pdfs, open_pdf, select_pages

__all__ = ["PdfDocument", "merge_pdfs", "open_pdf", "select_pages"]
