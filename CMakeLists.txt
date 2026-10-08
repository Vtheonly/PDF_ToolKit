cmake_minimum_required(VERSION 3.25)
project(PDF_ToolKit LANGUAGES NONE
        DESCRIPTION "pdftoolkit - unified headless PDF engine (Python + native)")

# Thin shim only: all C++ build logic lives under native/ (ADR-0002).
# The Python packaging pipeline (pyproject.toml/setuptools) is untouched
# by anything below this line.

enable_testing()
add_subdirectory(native)
