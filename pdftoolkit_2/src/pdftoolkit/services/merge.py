"""Merge service: explicit multi-file merges and whole-folder merges."""

from __future__ import annotations

from typing import Dict, List, Optional, Sequence

from ..core.errors import PdfReadError, ValidationError
from ..core.validation import resolve_path
from ..io.pdfio import merge_pdfs, open_pdf
from ..utils.files import find_pdfs
from .base import Service

DEFAULT_OUTPUT_NAME = "merged.pdf"
MIN_INPUTS = 2


class MergeService(Service):
    """Concatenate PDFs with strict input validation and idempotent folders."""

    def merge(self, inputs: Sequence, output) -> Dict:
        """Merge at least two documents (in order) into ``output``."""
        if inputs is None or len(inputs) < MIN_INPUTS:
            raise ValidationError(
                f"merge requires at least {MIN_INPUTS} input documents",
                {"provided": 0 if inputs is None else len(inputs)},
            )

        paths = [resolve_path(item, must_exist=True, kind="file") for item in inputs]
        target = resolve_path(output, must_exist=False, kind="any")

        result = merge_pdfs(paths, target)
        return {
            "output": result["output"],
            "files": result["files"],
            "pages": result["pages"],
            "sources": result["sources"],
        }

    def merge_folder(
        self,
        directory,
        output: Optional = None,
        recursive: bool = False,
    ) -> Dict:
        """Merge every PDF in ``directory`` in natural filename order.

        Hardening guarantees:

        * the default output (``merged.pdf`` inside the folder) is excluded
          from the candidate set, so re-running the operation is idempotent;
        * an explicit ``output`` is **never** excluded - pointing it at one
          of the candidates raises
          :class:`~pdftoolkit.core.errors.OutputConflictError` (checked in
          :func:`pdftoolkit.io.pdfio.merge_pdfs`);
        * unreadable PDFs are skipped and reported in ``skipped``.
        """
        root = resolve_path(directory, must_exist=True, kind="dir")
        default_target = root / DEFAULT_OUTPUT_NAME
        if output is None:
            target = default_target
            exclude = [default_target]
        else:
            target = resolve_path(output, must_exist=False, kind="any")
            exclude = []

        candidates = find_pdfs(root, exclude=exclude, recursive=recursive)

        readable: List = []
        skipped: List[Dict] = []
        for path in candidates:
            try:
                with open_pdf(path) as doc:
                    readable.append((path, doc.page_count))
            except PdfReadError:
                skipped.append({"path": str(path), "reason": "unreadable_pdf"})

        if len(readable) < MIN_INPUTS:
            raise ValidationError(
                f"merge_folder requires at least {MIN_INPUTS} readable PDFs, "
                f"found {len(readable)} in {root}",
                {"directory": str(root), "found": len(readable)},
            )

        result = merge_pdfs([path for path, _ in readable], target)
        return {
            "directory": str(root),
            "output": result["output"],
            "files": result["files"],
            "pages": result["pages"],
            "sources": [str(path) for path, _ in readable],
            "pages_per_source": [
                {"path": str(path), "pages": count} for path, count in readable
            ],
            "skipped": skipped,
            "recursive": bool(recursive),
        }
