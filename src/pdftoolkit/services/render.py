"""Rendering service: rasterise PDF pages to PNG images, files or thumbnails.

Consolidates the two original rendering behaviours:

* full-quality page rendering at a configurable DPI to base64 payloads or
  collision-free PNG files (render service of the previous unified engine,
  covering the original ``pdf_to_image`` utilities);
* lightweight thumbnail rendering at a scale factor, returned as
  ``data:image/png;base64,...`` URIs (the original pdf-search backend
  contract used for page previews).
"""

from __future__ import annotations

import base64
from typing import Dict, Optional

from ..core.errors import ValidationError
from ..core.pages import PageSpec, parse_page_spec
from ..core.validation import resolve_path
from ..io.pdfio import open_pdf
from ..utils.files import next_available_path
from ..utils.naming import render_filename, stem
from .base import Service

MIN_DPI = 36
MAX_DPI = 1200
DEFAULT_DPI = 150
MIN_SCALE = 0.05
MAX_SCALE = 8.0
DEFAULT_SCALE = 0.3

DATA_URI_PREFIX = "data:image/png;base64,"


class RenderService(Service):
    """Render pages to base64 PNG payloads, PNG files or data-URI thumbnails."""

    def render_pages(
        self,
        reference,
        pages: PageSpec = None,
        dpi: int = DEFAULT_DPI,
    ) -> Dict:
        """Return PNG bytes per page, base64-encoded for JSON transport."""
        dpi = self._validate_dpi(dpi)
        meta = self.registry.resolve(reference)

        images: Dict[str, str] = {}
        with open_pdf(meta.path) as doc:
            selected = parse_page_spec(pages, doc.page_count)
            zoom = dpi / 72.0
            for page in selected:
                png = doc.render_page(page, zoom=zoom)
                images[str(page)] = base64.b64encode(png).decode("ascii")
            page_count = doc.page_count

        return {
            "document": meta.to_dict(),
            "page_count": page_count,
            "dpi": dpi,
            "pages_rendered": len(selected),
            "images": images,
        }

    def render_thumbnails(
        self,
        reference,
        pages: PageSpec = None,
        scale: float = DEFAULT_SCALE,
    ) -> Dict:
        """Return lightweight page previews as ``data:image/png;base64`` URIs.

        This preserves the original pdf-search preview contract exactly: a
        ``scale`` factor (default ``0.3``) instead of DPI, and thumbnails
        encoded as ready-to-embed data URIs.
        """
        scale = self._validate_scale(scale)
        meta = self.registry.resolve(reference)

        thumbnails: Dict[str, str] = {}
        with open_pdf(meta.path) as doc:
            selected = parse_page_spec(pages, doc.page_count)
            for page in selected:
                png = doc.render_page(page, zoom=scale)
                encoded = base64.b64encode(png).decode("ascii")
                thumbnails[str(page)] = f"{DATA_URI_PREFIX}{encoded}"
            page_count = doc.page_count

        return {
            "document": meta.to_dict(),
            "page_count": page_count,
            "scale": scale,
            "pages_rendered": len(selected),
            "thumbnails": thumbnails,
        }

    def render_to_files(
        self,
        reference,
        output_dir: Optional = None,
        pages: PageSpec = None,
        dpi: int = DEFAULT_DPI,
        prefix: Optional[str] = None,
    ) -> Dict:
        """Render pages to PNG files, never overwriting existing files.

        Files are named ``{prefix|document-stem}-page-0001.png``; if a name
        already exists a collision-free ``(1)`` variant is used instead.
        """
        dpi = self._validate_dpi(dpi)
        meta = self.registry.resolve(reference)

        if output_dir is not None:
            directory = resolve_path(output_dir, must_exist=False, kind="any")
        else:
            directory = resolve_path(meta.path).parent

        effective_prefix = prefix if prefix else stem(meta.path)
        directory.mkdir(parents=True, exist_ok=True)

        written = []
        with open_pdf(meta.path) as doc:
            selected = parse_page_spec(pages, doc.page_count)
            zoom = dpi / 72.0
            for page in selected:
                candidate = directory / render_filename(effective_prefix, page)
                target = next_available_path(candidate)
                target.write_bytes(doc.render_page(page, zoom=zoom))
                written.append(str(target))
            page_count = doc.page_count

        return {
            "document": meta.to_dict(),
            "page_count": page_count,
            "dpi": dpi,
            "directory": str(directory),
            "files": written,
            "count": len(written),
        }

    # -- internals ----------------------------------------------------------

    @staticmethod
    def _validate_dpi(dpi) -> int:
        if isinstance(dpi, bool) or not isinstance(dpi, (int, float)):
            raise ValidationError(f"dpi must be a number, got {dpi!r}")
        dpi = int(dpi)
        if dpi < MIN_DPI or dpi > MAX_DPI:
            raise ValidationError(
                f"dpi must be between {MIN_DPI} and {MAX_DPI}, got {dpi}",
                {"dpi": dpi, "min": MIN_DPI, "max": MAX_DPI},
            )
        return dpi

    @staticmethod
    def _validate_scale(scale) -> float:
        if isinstance(scale, bool) or not isinstance(scale, (int, float)):
            raise ValidationError(f"scale must be a number, got {scale!r}")
        scale = float(scale)
        if scale < MIN_SCALE or scale > MAX_SCALE:
            raise ValidationError(
                f"scale must be between {MIN_SCALE} and {MAX_SCALE}, got {scale}",
                {"scale": scale, "min": MIN_SCALE, "max": MAX_SCALE},
            )
        return scale
