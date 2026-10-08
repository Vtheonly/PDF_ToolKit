#!/bin/bash
# Setup script for pdf-merge tool

set -e  # Exit on error

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
VENV_DIR="$SCRIPT_DIR/.venv"

echo "Setting up pdf-merge in $SCRIPT_DIR..."

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
else
    echo "Virtual environment already exists."
fi

source "$VENV_DIR/bin/activate"

echo "Installing dependencies..."
pip install --upgrade pip
if [ -f "$SCRIPT_DIR/requirements.txt" ]; then
    pip install -r "$SCRIPT_DIR/requirements.txt"
else
    echo "requirements.txt not found!"
    exit 1
fi

echo ""
echo "Setup complete!"
echo ""
echo "To add the command to your shell, add the following line to your ~/.bashrc:"
echo "alias pdf-merge=\"$VENV_DIR/bin/python $SCRIPT_DIR/pdf_merge.py\""
echo ""
echo "Then run: source ~/.bashrc"
