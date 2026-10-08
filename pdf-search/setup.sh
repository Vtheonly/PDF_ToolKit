#!/bin/bash
# Setup script for PDF Search Tool

set -e  # Exit on error

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$SCRIPT_DIR/backend"
FRONTEND_DIR="$SCRIPT_DIR/frontend"
VENV_DIR="$BACKEND_DIR/.venv"

echo "==========================================="
echo "PDF Search & Extract Tool - Setup"
echo "==========================================="
echo ""

# Create Python virtual environment
echo "[1/4] Setting up Python backend..."
echo ""

if [ ! -d "$VENV_DIR" ]; then
    echo "Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
else
    echo "Virtual environment already exists."
fi

source "$VENV_DIR/bin/activate"

echo "Installing Python dependencies..."
pip install --upgrade pip -q
pip install -r "$BACKEND_DIR/requirements.txt" -q

echo "✓ Python backend ready"
echo ""

# Install Node.js dependencies
echo "[2/4] Setting up Electron frontend..."
echo ""

cd "$FRONTEND_DIR"

if [ ! -d "node_modules" ]; then
    echo "Installing Node.js dependencies..."
    npm install --silent
else
    echo "Node modules already installed."
fi

echo "✓ Electron frontend ready"
echo ""

# Create temp directory
echo "[3/4] Creating temp directory..."
mkdir -p "$SCRIPT_DIR/temp"
echo "✓ Temp directory ready"
echo ""

# Add alias to .bashrc
echo "[4/4] Setting up shell alias..."
echo ""

BASHRC="$HOME/.bashrc"
ALIAS_LINE="alias pdf-search=\"cd $SCRIPT_DIR && ./start.sh\""
COMMENT_LINE="# PDF Search and Extraction Tool"

# Check if alias already exists
if grep -q "alias pdf-search=" "$BASHRC" 2>/dev/null; then
    echo "Alias already exists in .bashrc"
else
    echo "" >> "$BASHRC"
    echo "$COMMENT_LINE" >> "$BASHRC"
    echo "$ALIAS_LINE" >> "$BASHRC"
    echo "Added 'pdf-search' alias to .bashrc"
fi

echo "✓ Shell alias configured"
echo ""

echo "==========================================="
echo "Setup complete!"
echo "==========================================="
echo ""
echo "To start using the tool:"
echo "  1. Run: source ~/.bashrc"
echo "  2. Run: pdf-search"
echo ""
echo "Or start directly with:"
echo "  cd $SCRIPT_DIR && ./start.sh"
echo ""
