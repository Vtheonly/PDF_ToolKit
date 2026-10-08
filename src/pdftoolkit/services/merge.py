"""Merge service: explicit multi-file merges and whole-folder merges."""

from __future__ import annotations

import re
from typing import Dict, List, Optional, Sequence, Tuple

from ..core.errors import PdfReadError, ValidationError
from ..core.validation import resolve_path
from ..io.pdfio import merge_pdfs, open_pdf
from ..utils.files import find_pdfs
from ..utils.naming import sortable_key
from .base import Service

DEFAULT_OUTPUT_NAME = "merged.pdf"
MIN_INPUTS = 2

# The numeric-prefix pattern of the original pdfm tool: only files named
# ``<number>_<rest>.pdf`` participate, ordered by their leading number.
NUMERIC_PREFIX_PATTERN = re.compile(r"^(\d+)_(.+)$", re.IGNORECASE)


def numeric_prefix_order(path) -> Optional[Tuple[int, str]]:
    """Return ``(number, filename)`` when ``path`` matches ``N_rest.pdf``.

    Files not matching the pattern return ``None`` and are ignored by
    numeric-prefix folder merges, exactly like the original pdfm tool.
    """
    name = path.name
    match = NUMERIC_PREFIX_PATTERN.match(name[: -len(path.suffix)] if path.suffix else name)
    if match is None:
        return None
    return int(match.group(1)), name


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
        numeric_prefix: bool = False,
    ) -> Dict:
        """Merge every PDF in ``directory`` in natural filename order.

        Hardening guarantees:

        * the folder's own default output (``merged.pdf``) is **always**
          excluded from the candidate set - a stale ``merged.pdf`` from a
          previous run is never merged into itself, so re-running the
          operation is idempotent whichever output path is chosen;
        * pointing an explicit ``output`` at one of the remaining
          candidates raises
          :class:`~pdftoolkit.core.errors.OutputConflictError` (checked in
          :func:`pdftoolkit.io.pdfio.merge_pdfs`);
        * unreadable PDFs are skipped and reported in ``skipped``.

        With ``numeric_prefix=True`` the folder selection replicates the
        original pdfm tool: only files named ``<number>_<rest>.pdf`` are
        merged, ordered by their leading number (ties broken naturally),
        and non-matching PDFs are silently ignored.
        """
        root = resolve_path(directory, must_exist=True, kind="dir")
        default_target = root / DEFAULT_OUTPUT_NAME
        if output is None:
            target = default_target
        else:
            target = resolve_path(output, must_exist=False, kind="any")
        exclude = [default_target]

        candidates = find_pdfs(root, exclude=exclude, recursive=recursive)
        ignored: List[Dict] = []

        if numeric_prefix:
            selected: List = []
            for path in candidates:
                order = numeric_prefix_order(path)
                if order is None:
                    ignored.append({"path": str(path), "reason": "no_numeric_prefix"})
                else:
                    selected.append((order[0], order[1], path))
            selected.sort(key=lambda item: (item[0], sortable_key(item[1])))
            candidates = [path for _, _, path in selected]

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
            "numeric_prefix": bool(numeric_prefix),
            "ignored": ignored,
        }
