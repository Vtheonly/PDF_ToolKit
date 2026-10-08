#!/usr/bin/env python3
"""
Flask API Server for PDF Search Tool

Provides RESTful endpoints for:
- Uploading PDF files
- Searching keywords
- Getting page previews
- Exporting filtered PDFs
"""

import os
import uuid
import json
from datetime import datetime
from flask import Flask, request, jsonify, send_file
from flask_cors import CORS
from werkzeug.utils import secure_filename

from pdf_search import (
    search_keywords,
    get_match_summary,
    calculate_page_range,
    extract_pages,
    get_total_pages,
    render_page_thumbnail,
    render_all_thumbnails
)

app = Flask(__name__)
CORS(app)

# Configuration
UPLOAD_FOLDER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'temp')
ALLOWED_EXTENSIONS = {'pdf'}
MAX_CONTENT_LENGTH = 100 * 1024 * 1024  # 100MB max file size

app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER
app.config['MAX_CONTENT_LENGTH'] = MAX_CONTENT_LENGTH

# In-memory storage for uploaded files and search results
uploaded_files = {}  # file_id -> file_path
search_results = {}  # file_id -> search results


def allowed_file(filename: str) -> bool:
    """Check if file extension is allowed."""
    return '.' in filename and filename.rsplit('.', 1)[1].lower() in ALLOWED_EXTENSIONS


def ensure_upload_folder():
    """Ensure upload folder exists."""
    os.makedirs(UPLOAD_FOLDER, exist_ok=True)


@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check endpoint."""
    return jsonify({
        'status': 'ok',
        'timestamp': datetime.now().isoformat()
    })


@app.route('/api/upload', methods=['POST'])
def upload_pdf():
    """
    Upload a PDF file.
    
    Returns:
        JSON with file_id and metadata
    """
    ensure_upload_folder()
    
    if 'file' not in request.files:
        return jsonify({'error': 'No file provided'}), 400
    
    file = request.files['file']
    
    if file.filename == '':
        return jsonify({'error': 'No file selected'}), 400
    
    if not allowed_file(file.filename):
        return jsonify({'error': 'Only PDF files are allowed'}), 400
    
    # Generate unique file ID
    file_id = str(uuid.uuid4())
    filename = secure_filename(file.filename)
    file_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_{filename}")
    
    # Save file
    file.save(file_path)
    
    # Get metadata
    total_pages = get_total_pages(file_path)
    
    # Store file info
    uploaded_files[file_id] = {
        'path': file_path,
        'original_name': filename,
        'total_pages': total_pages
    }
    
    return jsonify({
        'file_id': file_id,
        'filename': filename,
        'total_pages': total_pages,
        'message': 'File uploaded successfully'
    })


@app.route('/api/search', methods=['POST'])
def search_pdf():
    """
    Search PDF for keywords.
    
    Expected JSON body:
        {
            "file_id": "...",
            "keywords": ["keyword1", "keyword2"],
            "case_sensitive": false
        }
    
    Returns:
        JSON with matched pages and counts
    """
    data = request.get_json()
    
    if not data:
        return jsonify({'error': 'No JSON data provided'}), 400
    
    file_id = data.get('file_id')
    keywords = data.get('keywords', [])
    case_sensitive = data.get('case_sensitive', False)
    
    if not file_id or file_id not in uploaded_files:
        return jsonify({'error': 'Invalid or missing file_id'}), 400
    
    if not keywords:
        return jsonify({'error': 'At least one keyword is required'}), 400
    
    file_info = uploaded_files[file_id]
    pdf_path = file_info['path']
    total_pages = file_info['total_pages']
    
    # Perform search
    matches = search_keywords(pdf_path, keywords, case_sensitive)
    
    # Calculate pages with padding
    matched_page_nums = [m.page_number for m in matches]
    pages_with_padding = calculate_page_range(matched_page_nums, total_pages, padding=2)
    
    # Build match details
    match_details = [
        {
            'page': m.page_number,
            'match_count': m.match_count,
            'keywords_found': m.keywords_found
        }
        for m in matches
    ]
    
    # Store results
    search_results[file_id] = {
        'keywords': keywords,
        'match_details': match_details,
        'matched_pages': matched_page_nums,
        'pages_with_padding': pages_with_padding
    }
    
    return jsonify({
        'file_id': file_id,
        'total_pages': total_pages,
        'keywords': keywords,
        'match_details': match_details,
        'matched_pages': matched_page_nums,
        'pages_with_padding': pages_with_padding,
        'total_matched_pages': len(matched_page_nums),
        'total_output_pages': len(pages_with_padding)
    })


@app.route('/api/preview/<file_id>/<int:page_num>', methods=['GET'])
def get_page_preview(file_id: str, page_num: int):
    """
    Get thumbnail preview for a specific page.
    
    Args:
        file_id: The uploaded file ID
        page_num: Page number (1-indexed)
    
    Returns:
        JSON with base64-encoded image
    """
    if file_id not in uploaded_files:
        return jsonify({'error': 'Invalid file_id'}), 400
    
    file_info = uploaded_files[file_id]
    pdf_path = file_info['path']
    total_pages = file_info['total_pages']
    
    if page_num < 1 or page_num > total_pages:
        return jsonify({'error': f'Invalid page number. Must be 1-{total_pages}'}), 400
    
    try:
        thumbnail = render_page_thumbnail(pdf_path, page_num, scale=0.3)
        return jsonify({
            'page': page_num,
            'thumbnail': thumbnail
        })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/previews/<file_id>', methods=['POST'])
def get_page_previews(file_id: str):
    """
    Get thumbnail previews for multiple pages.
    
    Expected JSON body:
        {
            "pages": [1, 2, 3, ...]  // Optional, defaults to all padded pages
        }
    
    Returns:
        JSON with page thumbnails
    """
    if file_id not in uploaded_files:
        return jsonify({'error': 'Invalid file_id'}), 400
    
    file_info = uploaded_files[file_id]
    pdf_path = file_info['path']
    
    data = request.get_json() or {}
    pages = data.get('pages')
    
    if pages is None and file_id in search_results:
        # Default to pages with padding from last search
        pages = search_results[file_id]['pages_with_padding']
    
    try:
        thumbnails = render_all_thumbnails(pdf_path, pages, scale=0.3)
        return jsonify({
            'file_id': file_id,
            'thumbnails': thumbnails
        })
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/export', methods=['POST'])
def export_pdf():
    """
    Export selected pages to a new PDF.
    
    Expected JSON body:
        {
            "file_id": "...",
            "pages": [1, 3, 5, ...],  // Pages to include (1-indexed)
            "filename": "output.pdf"  // Optional output filename
        }
    
    Returns:
        The generated PDF file
    """
    data = request.get_json()
    
    if not data:
        return jsonify({'error': 'No JSON data provided'}), 400
    
    file_id = data.get('file_id')
    pages = data.get('pages', [])
    output_filename = data.get('filename', 'filtered_output.pdf')
    
    if not file_id or file_id not in uploaded_files:
        return jsonify({'error': 'Invalid or missing file_id'}), 400
    
    if not pages:
        return jsonify({'error': 'No pages selected for export'}), 400
    
    file_info = uploaded_files[file_id]
    pdf_path = file_info['path']
    
    # Generate output path
    output_path = os.path.join(UPLOAD_FOLDER, f"export_{file_id}_{output_filename}")
    
    try:
        extract_pages(pdf_path, pages, output_path)
        
        return send_file(
            output_path,
            as_attachment=True,
            download_name=output_filename,
            mimetype='application/pdf'
        )
    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/api/cleanup/<file_id>', methods=['DELETE'])
def cleanup_file(file_id: str):
    """
    Clean up uploaded file and associated data.
    """
    if file_id not in uploaded_files:
        return jsonify({'error': 'Invalid file_id'}), 400
    
    file_info = uploaded_files[file_id]
    
    # Remove file
    if os.path.exists(file_info['path']):
        os.remove(file_info['path'])
    
    # Clean up export files
    for f in os.listdir(UPLOAD_FOLDER):
        if f.startswith(f"export_{file_id}"):
            os.remove(os.path.join(UPLOAD_FOLDER, f))
    
    # Remove from memory
    del uploaded_files[file_id]
    if file_id in search_results:
        del search_results[file_id]
    
    return jsonify({'message': 'Cleanup successful'})


if __name__ == '__main__':
    ensure_upload_folder()
    print("Starting PDF Search API Server...")
    print(f"Upload folder: {UPLOAD_FOLDER}")
    app.run(host='127.0.0.1', port=5050, debug=False)
