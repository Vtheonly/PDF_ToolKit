#!/bin/bash
# Start script for PDF Search Tool
# Launches Python backend and Electron frontend

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$SCRIPT_DIR/backend"
FRONTEND_DIR="$SCRIPT_DIR/frontend"
VENV_PYTHON="$BACKEND_DIR/.venv/bin/python"

# Cleanup function
cleanup() {
    echo ""
    echo "Shutting down..."
    
    # Kill backend if running
    if [ ! -z "$BACKEND_PID" ]; then
        kill $BACKEND_PID 2>/dev/null
    fi
    
    # Kill any remaining python server processes
    pkill -f "server.py" 2>/dev/null
    
    exit 0
}

# Set up trap for cleanup on exit
trap cleanup EXIT INT TERM

# Check if setup was run
if [ ! -d "$BACKEND_DIR/.venv" ]; then
    echo "Error: Backend virtual environment not found."
    echo "Please run ./setup.sh first."
    exit 1
fi

if [ ! -d "$FRONTEND_DIR/node_modules" ]; then
    echo "Error: Node modules not found."
    echo "Please run ./setup.sh first."
    exit 1
fi

echo "Starting PDF Search & Extract Tool..."
echo ""

# Start Python backend in background
echo "Starting backend server..."
cd "$BACKEND_DIR"
$VENV_PYTHON server.py &
BACKEND_PID=$!

# Wait for backend to start
sleep 2

# Check if backend is running
if ! kill -0 $BACKEND_PID 2>/dev/null; then
    echo "Error: Backend failed to start."
    exit 1
fi

echo "Backend running on http://127.0.0.1:5050"
echo ""

# Start Electron frontend
echo "Starting Electron app..."
cd "$FRONTEND_DIR"
npm start

# Cleanup will be called automatically on exit
