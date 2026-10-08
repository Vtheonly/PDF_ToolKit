"""CLI adapter tests for the commands and flags added by the unification."""

import json

import pytest

from pdftoolkit.api import cli
from pdftoolkit import Toolkit


class FakeToolkit:
    instances = []

    def __init__(self, strict=False):
        self.strict = strict
        self.calls = []
        FakeToolkit.instances.append(self)

    def __getattr__(self, name):
        def method(*args, **kwargs):
            self.calls.append((name, args, kwargs))
            from pdftoolkit.core.result import success

            return success(name, {"echo": [list(args), kwargs]})

        return method


@pytest.fixture(autouse=True)
def fake_cli_toolkit(monkeypatch):
    FakeToolkit.instances = []
    monkeypatch.setattr(cli, "Toolkit", FakeToolkit)


def run_cli(argv):
    import io
    from contextlib import redirect_stdout

    buffer = io.StringIO()
    with redirect_stdout(buffer):
        code = cli.main(argv)
    return code, buffer.getvalue()


def last_toolkit():
    return FakeToolkit.instances[-1]


class TestNewSearchFlags:
    def test_search_case_sensitive_flag(self):
        run_cli(["search", "doc-1", "alpha", "--case-sensitive"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "search"
        assert kwargs["case_sensitive"] is True

    def test_search_substring_flag(self):
        run_cli(["search", "doc-1", "alpha", "--substring"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["whole_words"] is False

    def test_search_default_flags(self):
        run_cli(["search", "doc-1", "alpha"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["case_sensitive"] is False
        assert kwargs["whole_words"] is True

    def test_corpus_recursive_flag(self):
        run_cli(["search-corpus", "alpha", "--recursive"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["recursive"] is True

    def test_corpus_rank_flag(self):
        run_cli(["search-corpus", "alpha", "--rank"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["sort"] == "rank"

    def test_corpus_default_sort_is_path(self):
        run_cli(["search-corpus", "alpha"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["sort"] == "path"


class TestExtractMatchesCommand:
    def test_dispatch_with_defaults(self):
        run_cli(["extract-matches", "doc-1", "alpha", "beta", "out.pdf"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "extract_matches"
        assert args == ("doc-1", ["alpha", "beta"], "out.pdf")
        assert kwargs == {
            "padding": 2,
            "pages": None,
            "case_sensitive": False,
            "whole_words": True,
        }

    def test_dispatch_with_options(self):
        run_cli(
            [
                "extract-matches", "doc-1", "alpha", "out.pdf",
                "--padding", "3",
                "--pages", "1-10",
                "--case-sensitive",
                "--substring",
            ]
        )
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["padding"] == 3
        assert kwargs["pages"] == "1-10"
        assert kwargs["case_sensitive"] is True
        assert kwargs["whole_words"] is False


class TestThumbnailsCommand:
    def test_dispatch_with_defaults(self):
        run_cli(["thumbnails", "doc-1"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "render_thumbnails"
        assert args == ("doc-1",)
        assert kwargs == {"pages": None, "scale": 0.3}

    def test_dispatch_with_options(self):
        run_cli(["thumbnails", "doc-1", "--pages", "2-4", "--scale", "0.5"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs == {"pages": "2-4", "scale": 0.5}


class TestMergeFolderNumericPrefix:
    def test_numeric_prefix_flag(self):
        run_cli(["merge-folder", "docs/", "--numeric-prefix"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["numeric_prefix"] is True

    def test_default_off(self):
        run_cli(["merge-folder", "docs/"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["numeric_prefix"] is False


class TestSpeakVoiceControls:
    def test_rate_and_volume_flags(self):
        run_cli(["speak", "hello", "--aloud", "--rate", "200", "--volume", "1.0"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["rate"] == 200.0
        assert kwargs["volume"] == 1.0

    def test_defaults_are_none(self):
        run_cli(["speak", "hello", "--aloud"])
        kwargs = last_toolkit().calls[0][2]
        assert kwargs["rate"] is None
        assert kwargs["volume"] is None


class TestRealEngineCommands:
    """Smoke tests against the real toolkit."""

    @pytest.fixture
    def real_cli(self, monkeypatch, speech_provider):
        monkeypatch.setattr(
            cli,
            "Toolkit",
            lambda strict=False: Toolkit(strict=strict, speech_provider=speech_provider),
        )
        return run_cli

    def test_real_thumbnails(self, real_cli, sample_pdf):
        code, output = real_cli(["thumbnails", str(sample_pdf), "--pages", "1"])
        envelope = json.loads(output)
        assert code == 0
        assert envelope["data"]["thumbnails"]["1"].startswith("data:image/png;base64,")

    def test_real_extract_matches(self, real_cli, sample_pdf, tmp_path):
        out = tmp_path / "matches.pdf"
        code, output = real_cli(
            ["extract-matches", str(sample_pdf), "alpha", str(out), "--padding", "1"]
        )
        envelope = json.loads(output)
        assert code == 0
        assert envelope["data"]["pages_with_padding"] == [1, 2, 3, 4]
        assert out.is_file()

    def test_real_case_sensitive_search(self, real_cli, tmp_path):
        from helpers import make_pdf

        pdf = make_pdf(tmp_path / "cased.pdf", ["Quick fox", "quick fox"])
        code, output = real_cli(["search", str(pdf), "quick", "--case-sensitive"])
        envelope = json.loads(output)
        assert code == 0
        assert envelope["data"]["totals"] == {"quick": 1}

    def test_real_corpus_rank(self, real_cli, tmp_path):
        from helpers import make_pdf

        corpus = tmp_path / "corpus"
        corpus.mkdir()
        make_pdf(corpus / "a_few.pdf", ["alpha"])
        make_pdf(corpus / "b_many.pdf", ["alpha alpha alpha"])
        code, output = real_cli(["search-corpus", "alpha", "--directory", str(corpus), "--rank"])
        envelope = json.loads(output)
        assert code == 0
        assert envelope["data"]["documents"][0]["document"]["filename"] == "b_many.pdf"

    def test_real_numeric_prefix_merge(self, real_cli, tmp_path):
        from helpers import make_pdf

        folder = tmp_path / "pdfm"
        folder.mkdir()
        make_pdf(folder / "10_zeta.pdf", ["zeta"])
        make_pdf(folder / "2_beta.pdf", ["beta"])
        make_pdf(folder / "1_alpha.pdf", ["alpha"])
        code, output = real_cli(["merge-folder", str(folder), "--numeric-prefix"])
        envelope = json.loads(output)
        assert code == 0
        assert [p.rsplit("/", 1)[-1] for p in envelope["data"]["sources"]] == [
            "1_alpha.pdf",
            "2_beta.pdf",
            "10_zeta.pdf",
        ]
