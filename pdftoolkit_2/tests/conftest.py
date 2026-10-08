"""Shared pytest fixtures for the whole suite."""

from __future__ import annotations

import pytest

from helpers import (
    FakeSpeechProvider,
    make_corrupt_pdf,
    make_links_pdf,
    make_pdf,
    write_text_file,
)
from pdftoolkit import Toolkit


@pytest.fixture
def speech_provider() -> FakeSpeechProvider:
    return FakeSpeechProvider()


@pytest.fixture
def engine(speech_provider) -> Toolkit:
    """Non-strict engine with an in-memory speech provider."""
    return Toolkit(strict=False, speech_provider=speech_provider)


@pytest.fixture
def strict_engine(speech_provider) -> Toolkit:
    """Strict engine: typed errors propagate instead of envelopes."""
    return Toolkit(strict=True, speech_provider=speech_provider)


@pytest.fixture
def sample_pdf(tmp_path):
    """5-page PDF with known keyword distribution.

    Page contents: alpha beta / gamma delta / alpha alpha / epsilon / beta gamma
    Keyword totals: alpha=3 (pages 1, 3x2), beta=2 (pages 1, 5), gamma=2
    (pages 2, 5), delta=1, epsilon=1.
    """
    return make_pdf(
        tmp_path / "sample.pdf",
        ["alpha beta", "gamma delta", "alpha alpha", "epsilon", "beta gamma"],
    )


@pytest.fixture
def links_pdf(tmp_path):
    return make_links_pdf(tmp_path / "links.pdf")


@pytest.fixture
def corrupt_pdf(tmp_path):
    return make_corrupt_pdf(tmp_path / "corrupt.pdf")


@pytest.fixture
def corpus_dir(tmp_path):
    """Folder with readable PDFs, an unrelated text file and a broken PDF."""
    root = tmp_path / "corpus"
    root.mkdir()
    make_pdf(root / "lex_one.pdf", ["alpha"])
    make_pdf(root / "lex_three.pdf", ["alpha", "beta", "gamma"])
    make_pdf(root / "broken.pdf", ["ok"])  # replaced with junk below
    make_corrupt_pdf(root / "broken.pdf")
    write_text_file(root / "unrelated.txt", "alpha beta gamma")
    return root


@pytest.fixture
def merge_dir(tmp_path):
    """Folder whose PDFs only merge correctly under natural sort order."""
    root = tmp_path / "merge"
    root.mkdir()
    make_pdf(root / "1_alpha.pdf", ["alpha one", "alpha two"])
    make_pdf(root / "2_beta.pdf", ["beta one"])
    make_pdf(root / "10_zeta.pdf", ["zeta one", "zeta two", "zeta three"])
    make_pdf(root / "merged.pdf", ["stale output that must be excluded"])
    write_text_file(root / "notes.txt", "not a pdf")
    return root


@pytest.fixture
def multi_pdf(tmp_path):
    """Three single-page PDFs A/B/C for merge-order assertions."""
    return [
        make_pdf(tmp_path / "a.pdf", ["A page"]),
        make_pdf(tmp_path / "b.pdf", ["B page"]),
        make_pdf(tmp_path / "c.pdf", ["C page"]),
    ]
