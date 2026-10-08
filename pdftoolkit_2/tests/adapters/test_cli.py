"""Adapter tests for the JSON command-line interface."""

import json

import pytest

from helpers import FakeSpeechProvider
from pdftoolkit.api import cli
from pdftoolkit.core.errors import DocumentNotFoundError
from pdftoolkit.core.result import failure, success
from pdftoolkit import Toolkit


class FakeToolkit:
    """Records dispatched calls; returns canned envelopes.

    Set ``FakeToolkit.fail_next`` to a typed error to make the next call
    return a failure envelope instead.
    """

    instances = []
    fail_next = None

    def __init__(self, strict=False):
        self.strict = strict
        self.calls = []
        FakeToolkit.instances.append(self)

    def __getattr__(self, name):
        def method(*args, **kwargs):
            self.calls.append((name, args, kwargs))
            if FakeToolkit.fail_next is not None:
                error = FakeToolkit.fail_next
                FakeToolkit.fail_next = None
                return failure(name, error)
            return success(name, {"echo": [list(args), kwargs]})

        return method


@pytest.fixture(autouse=True)
def fake_cli_toolkit(monkeypatch):
    FakeToolkit.instances = []
    FakeToolkit.fail_next = None
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


class TestCliEnvelopeContract:
    def test_success_exits_zero_with_json_envelope(self):
        code, output = run_cli(["capabilities"])
        assert code == 0
        envelope = json.loads(output)
        assert envelope["ok"] is True
        assert envelope["operation"] == "capabilities"

    def test_failure_exits_one(self):
        FakeToolkit.fail_next = DocumentNotFoundError("gone")
        code, output = run_cli(["info", "doc-ghost"])
        envelope = json.loads(output)
        assert code == 1
        assert envelope["ok"] is False
        assert envelope["error"]["code"] == "document_not_found"

    def test_no_command_prints_help_and_exits_two(self):
        code, output = run_cli([])
        assert code == 2
        assert "usage" in output.lower()

    def test_strict_flag_forwarded(self):
        run_cli(["--strict", "capabilities"])
        assert last_toolkit().strict is True


class TestCliDispatch:
    def test_register(self):
        run_cli(["register", "a.pdf"])
        assert last_toolkit().calls[0][0] == "register"
        assert last_toolkit().calls[0][1] == ("a.pdf",)

    def test_search_dispatch(self):
        run_cli(["search", "doc-1", "alpha", "beta", "--pages", "1-3"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "search"
        assert args == ("doc-1", ["alpha", "beta"])
        assert kwargs == {"pages": "1-3"}

    def test_search_corpus_dispatch(self):
        run_cli(["search-corpus", "alpha", "--directory", "corpus/"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "search_corpus"
        assert args == (["alpha"],)
        assert kwargs == {"directory": "corpus/", "pages": None}

    def test_text_dispatch(self):
        run_cli(["text", "doc-1", "--pages", "2"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "extract_text"
        assert kwargs == {"pages": "2"}

    def test_links_dispatch(self):
        run_cli(["links", "doc-1"])
        assert last_toolkit().calls[0][0] == "extract_links"

    def test_cut_dispatch(self):
        run_cli(["cut", "doc-1", "1-3", "out.pdf"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "cut_pages"
        assert args == ("doc-1", "1-3", "out.pdf")

    def test_context_anchor_page(self):
        run_cli(["context", "doc-1", "4", "--before", "2", "--after", "0"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "context_pages"
        assert args[1] == 4
        assert kwargs == {"before": 2, "after": 0}

    def test_context_anchor_keyword(self):
        run_cli(["context", "doc-1", "invoice"])
        name, args, _ = last_toolkit().calls[0]
        assert name == "context_pages"
        assert args[1] == "invoice"

    def test_extract_context_dispatch(self):
        run_cli(["extract-context", "doc-1", "invoice", "out.pdf"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "extract_context"
        assert args == ("doc-1", "invoice", "out.pdf")

    def test_merge_dispatch(self):
        run_cli(["merge", "a.pdf", "b.pdf", "out.pdf"])
        name, args, _ = last_toolkit().calls[0]
        assert name == "merge"
        assert args == (["a.pdf", "b.pdf"], "out.pdf")

    def test_merge_folder_dispatch(self):
        run_cli(["merge-folder", "docs/", "--output", "x.pdf", "--recursive"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "merge_folder"
        assert args == ("docs/",)
        assert kwargs == {"output": "x.pdf", "recursive": True}

    def test_render_dispatch(self):
        run_cli(["render", "doc-1", "--pages", "1", "--dpi", "200"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "render_pages"
        assert kwargs == {"pages": "1", "dpi": 200}

    def test_render_files_dispatch(self):
        run_cli(["render-files", "doc-1", "--output-dir", "out/", "--prefix", "p"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "render_to_files"
        assert kwargs == {"output_dir": "out/", "pages": None, "dpi": 150, "prefix": "p"}

    def test_speak_save_mode(self):
        run_cli(["speak", "hello world", "--save-path", "a.wav"])
        name, args, kwargs = last_toolkit().calls[0]
        assert name == "synthesize_speech"
        assert args == ("hello world",)
        assert kwargs == {"save_path": "a.wav", "speak_aloud": False}

    def test_speak_aloud_mode(self):
        run_cli(["speak", "hello", "--aloud"])
        name, args, kwargs = last_toolkit().calls[0]
        assert kwargs == {"save_path": None, "speak_aloud": True}

    def test_speech_requires_exactly_one_mode(self):
        from contextlib import redirect_stderr
        import io as io_module

        buffer = io_module.StringIO()
        try:
            with redirect_stderr(buffer):
                cli.main(["speak", "hello"])
        except SystemExit as exc:
            assert exc.code == 2

    def test_document_management_dispatch(self):
        run_cli(["list"])
        assert last_toolkit().calls[0][0] == "list_documents"
        run_cli(["remove", "doc-1"])
        assert last_toolkit().calls[-1][0] == "remove_document"


class TestCliWithRealEngine:
    """A small smoke layer against the real toolkit (no monkeypatching)."""

    def test_real_capabilities(self, monkeypatch):
        monkeypatch.setattr(
            cli,
            "Toolkit",
            lambda strict=False: Toolkit(strict=strict, speech_provider=FakeSpeechProvider()),
        )
        code, output = run_cli(["capabilities"])
        envelope = json.loads(output)
        assert code == 0
        assert envelope["data"]["engine"]["name"] == "pdftoolkit"

    def test_real_search_workflow(self, monkeypatch, sample_pdf):
        monkeypatch.setattr(
            cli,
            "Toolkit",
            lambda strict=False: Toolkit(strict=strict, speech_provider=FakeSpeechProvider()),
        )
        code, output = run_cli(["search", str(sample_pdf), "alpha"])
        envelope = json.loads(output)
        assert code == 0
        assert envelope["data"]["totals"] == {"alpha": 3}
