"""Adapter layer: thin, stateless surfaces over the :class:`Toolkit` facade.

``create_app`` is intentionally lazy so importing :mod:`pdftoolkit.api`
never requires FastAPI to be installed.
"""

from __future__ import annotations


def create_app():
    """Create the FastAPI application (requires the ``http`` extra)."""
    from .http import create_app as _create_app

    return _create_app()


__all__ = ["create_app"]
