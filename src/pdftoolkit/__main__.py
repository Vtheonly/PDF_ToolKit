"""Module entry point: ``python -m pdftoolkit`` runs the JSON CLI."""

from __future__ import annotations

import sys

from .api.cli import main

if __name__ == "__main__":
    sys.exit(main())
