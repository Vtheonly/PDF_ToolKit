import os
import glob
import subprocess
from flask import Flask, request, Response, escape

# Calculate VIEWS_DIR relative to this file's location
# Example: if app.py is in /home/user/project/PDF Search Engine/app.py,
# then os.path.dirname(os.path.abspath(__file__)) is /home/user/project/PDF Search Engine
# and VIEWS_DIR becomes /home/user/project/PDF Search Engine/views
VIEWS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'views')

# Configure Flask:
# - static_folder=VIEWS_DIR: Serve static files from the 'views' directory.
# - static_url_path='': Serve these files from the root URL path (e.g., views/style.css -> /style.css).
# Flask will also automatically serve 'index.html' from static_folder for '/' if it exists.
app = Flask(__name__, static_folder=VIEWS_DIR, static_url_path='')

PORT = 3000

@app.route('/search', methods=['POST'])
def search():
    directory = request.form.get('directory')
    keyword = request.form.get('keyword')

    if not directory or not keyword:
        return Response("Directory and keyword are required.", status=400)
    
    try:
        # Construct search pattern for glob
        # e.g., if directory is './docs', pattern is './docs/**/*.pdf'
        search_pattern = os.path.join(directory, '**', '*.pdf')
        files = glob.glob(search_pattern, recursive=True)
    except Exception as e:
        print(f"Glob error: {e}")
        return Response(f"Error during file search: {escape(str(e))}", status=500)

    if not files:
        return Response(f"No PDF files found in the specified directory: {escape(directory)}", status=404)

    # Prepare the command for pdfgrep
    # ['pdfgrep', 'search_keyword', 'file1.pdf', 'file2.pdf', ...]
    command = ['pdfgrep', keyword] + files

    try:
        process = subprocess.run(command, capture_output=True, text=True, check=False)
        
        # pdfgrep exit codes:
        # 0: At least one match was found.
        # 1: No match was found.
        # 2: An error occurred (e.g., file not readable, not a PDF, invalid options).
        
        if process.returncode == 2: # pdfgrep specific error
            error_message = process.stderr.strip() if process.stderr.strip() else "An unknown error occurred with pdfgrep."
            print(f"pdfgrep execution error (exit code 2): {error_message}")
            return Response(f"Error processing PDF files: {escape(error_message)}", status=500)
        
        # For exit codes 0 (match) and 1 (no match), stdout contains the result.
        # stdout is empty if no match (exit code 1).
        # The strip() handles potential trailing newlines from pdfgrep output.
        final_display_output = process.stdout.strip() if process.stdout.strip() else 'No matches found'
        
        go_back_link = '<br><a href="/">Go back</a>'

        html_response = f"""
            <!DOCTYPE html>
            <html lang="en">
            <head>
                <meta charset="UTF-8">
                <meta name="viewport" content="width=device-width, initial-scale=1.0">
                <title>Search Results</title>
            </head>
            <body>
                <h3>Search Results for "{escape(keyword)}":</h3>
                <pre>{escape(final_display_output)}</pre>
                {go_back_link}
            </body>
            </html>
        """
        return Response(html_response, mimetype='text/html')

    except FileNotFoundError:
        # This occurs if 'pdfgrep' command is not found in PATH
        print("Error: pdfgrep command not found.")
        return Response("Error: pdfgrep command not found. Please ensure it is installed and in your PATH.", status=500)
    except Exception as e: # Catch-all for other subprocess or unexpected errors
        print(f"Error executing pdfgrep or processing results: {e}")
        return Response(f"An unexpected error occurred: {escape(str(e))}", status=500)

if __name__ == '__main__':
    # debug=True enables auto-reloading and provides a debugger.
    # host='0.0.0.0' makes the server accessible externally (optional).
    app.run(debug=True, port=PORT)