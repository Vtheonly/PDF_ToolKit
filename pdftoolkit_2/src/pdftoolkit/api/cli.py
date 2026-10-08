"""JSON command-line adapter.

Every subcommand prints exactly one JSON envelope to stdout and exits with
status ``0`` on success and ``1`` on failure, making the CLI safe to script
and trivial to wrap.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import List, Optional, Sequence

from ..toolkit import Toolkit


def build_parser() -> argparse.ArgumentParser:
    """Construct the full argparse CLI surface."""
    parser = argparse.ArgumentParser(
        prog="pdftoolkit",
        description="Unified headless PDF engine - JSON in, JSON out.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="raise errors instead of returning failure envelopes",
    )
    sub = parser.add_subparsers(dest="command", metavar="COMMAND")

    def command(name: str, help_text: str) -> argparse.ArgumentParser:
        return sub.add_parser(name, help=help_text)

    p = command("capabilities", "List engine capabilities")
    p = command("register", "Register a document")
    p.add_argument("path")
    p = command("list", "List registered documents")
    p = command("info", "Show one document")
    p.add_argument("document")
    p = command("remove", "Remove a document")
    p.add_argument("document")

    p = command("search", "Search one document for keywords")
    p.add_argument("document")
    p.add_argument("keywords", nargs="+")
    p.add_argument("--pages", default=None)
    p = command("search-corpus", "Search all documents or a folder")
    p.add_argument("keywords", nargs="+")
    p.add_argument("--directory", default=None)
    p.add_argument("--pages", default=None)

    p = command("text", "Extract text")
    p.add_argument("document")
    p.add_argument("--pages", default=None)
    p = command("links", "Extract hyperlinks")
    p.add_argument("document")
    p.add_argument("--pages", default=None)

    p = command("cut", "Cut pages into a new PDF")
    p.add_argument("document")
    p.add_argument("pages")
    p.add_argument("output")
    p = command("context", "Show pages around a keyword or page")
    p.add_argument("document")
    p.add_argument("anchor")
    p.add_argument("--before", type=int, default=1)
    p.add_argument("--after", type=int, default=1)
    p = command("extract-context", "Extract context pages into a new PDF")
    p.add_argument("document")
    p.add_argument("anchor")
    p.add_argument("output")
    p.add_argument("--before", type=int, default=1)
    p.add_argument("--after", type=int, default=1)

    p = command("merge", "Merge documents")
    p.add_argument("inputs", nargs="+")
    p.add_argument("output")
    p = command("merge-folder", "Merge every PDF in a folder")
    p.add_argument("directory")
    p.add_argument("--output", default=None)
    p.add_argument("--recursive", action="store_true")

    p = command("render", "Render pages to base64 PNG payloads")
    p.add_argument("document")
    p.add_argument("--pages", default=None)
    p.add_argument("--dpi", type=int, default=150)
    p = command("render-files", "Render pages to PNG files")
    p.add_argument("document")
    p.add_argument("--output-dir", default=None)
    p.add_argument("--pages", default=None)
    p.add_argument("--dpi", type=int, default=150)
    p.add_argument("--prefix", default=None)

    p = command("speak", "Text-to-speech (exactly one output mode)")
    p.add_argument("text")
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--save-path", default=None)
    group.add_argument("--aloud", action="store_true")

    return parser


def _dispatch(toolkit: Toolkit, args: argparse.Namespace):
    """Map a parsed namespace onto a toolkit call returning an envelope."""
    if args.command == "capabilities":
        return toolkit.capabilities()
    if args.command == "register":
        return toolkit.register(args.path)
    if args.command == "list":
        return toolkit.list_documents()
    if args.command == "info":
        return toolkit.document_info(args.document)
    if args.command == "remove":
        return toolkit.remove_document(args.document)
    if args.command == "search":
        return toolkit.search(args.document, args.keywords, pages=args.pages)
    if args.command == "search-corpus":
        return toolkit.search_corpus(args.keywords, directory=args.directory, pages=args.pages)
    if args.command == "text":
        return toolkit.extract_text(args.document, pages=args.pages)
    if args.command == "links":
        return toolkit.extract_links(args.document, pages=args.pages)
    if args.command == "cut":
        return toolkit.cut_pages(args.document, args.pages, args.output)
    if args.command == "context":
        return _context(toolkit, args)
    if args.command == "extract-context":
        return _extract_context(toolkit, args)
    if args.command == "merge":
        return toolkit.merge(args.inputs, args.output)
    if args.command == "merge-folder":
        return toolkit.merge_folder(args.directory, output=args.output, recursive=args.recursive)
    if args.command == "render":
        return toolkit.render_pages(args.document, pages=args.pages, dpi=args.dpi)
    if args.command == "render-files":
        return toolkit.render_to_files(
            args.document,
            output_dir=args.output_dir,
            pages=args.pages,
            dpi=args.dpi,
            prefix=args.prefix,
        )
    if args.command == "speak":
        return toolkit.synthesize_speech(
            args.text, save_path=args.save_path, speak_aloud=bool(args.aloud)
        )
    raise ValueError(f"unknown command: {args.command}")


def _anchor(value: str) -> "int | str":
    """Accept either a page number or a keyword as anchor."""
    try:
        return int(value)
    except ValueError:
        return value


def _context(toolkit: Toolkit, args: argparse.Namespace):
    return toolkit.context_pages(
        args.document, _anchor(args.anchor), before=args.before, after=args.after
    )


def _extract_context(toolkit: Toolkit, args: argparse.Namespace):
    return toolkit.extract_context(
        args.document,
        _anchor(args.anchor),
        args.output,
        before=args.before,
        after=args.after,
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    """Run the CLI, print one JSON envelope and return the exit status."""
    parser = build_parser()
    args = parser.parse_args(argv)

    if not args.command:
        parser.print_help()
        return 2

    toolkit = Toolkit(strict=args.strict)
    envelope = _dispatch(toolkit, args)
    json.dump(envelope, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0 if envelope.get("ok") else 1


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
