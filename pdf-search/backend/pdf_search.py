#!/usr/bin/env python3
"""
PDF Search and Extraction Module

Provides functionality to:
- Search PDFs for keyword occurrences
- Track page matches with counts
- Apply padding windows around matched pages
- Remove duplicate/overlapping pages
- Extract pages to generate filtered PDFs
- Render page thumbnails for preview
"""

import os
import re
import base64
import io
from typing import Dict, List, Tuple, Set
from dataclasses import dataclass
from pypdf import PdfReader, PdfWriter
import fitz  # PyMuPDF


@dataclass
class MatchResult:
    """Represents keyword matches on a specific page."""
    page_number: int
    match_count: int
    keywords_found: List[str]


def search_keywords(pdf_path: str, keywords: List[str], case_sensitive: bool = False) -> List[MatchResult]:
    """
    Scan entire PDF for keyword occurrences.
    
    Args:
        pdf_path: Path to the PDF file
        keywords: List of keywords to search for
        case_sensitive: Whether search should be case-sensitive
        
    Returns:
        List of MatchResult objects for pages with matches
    """
    if not os.path.exists(pdf_path):
        raise FileNotFoundError(f"PDF file not found: {pdf_path}")
    
    if not keywords:
        raise ValueError("At least one keyword is required")
    
    results = []
    doc = fitz.open(pdf_path)
    
    try:
        for page_num in range(len(doc)):
            page = doc[page_num]
            text = page.get_text()
            
            if not case_sensitive:
                text_lower = text.lower()
            
            page_match_count = 0
            keywords_on_page = []
            
            for keyword in keywords:
                if case_sensitive:
                    pattern = re.compile(re.escape(keyword))
                else:
                    pattern = re.compile(re.escape(keyword), re.IGNORECASE)
                
                matches = pattern.findall(text if case_sensitive else text)
                # For case insensitive, we need to count differently
                if not case_sensitive:
                    matches = pattern.findall(text)
                
                count = len(matches)
                if count > 0:
                    page_match_count += count
                    keywords_on_page.append(keyword)
            
            if page_match_count > 0:
                results.append(MatchResult(
                    page_number=page_num + 1,  # 1-indexed
                    match_count=page_match_count,
                    keywords_found=keywords_on_page
                ))
    finally:
        doc.close()
    
    return results


def get_match_summary(results: List[MatchResult]) -> Dict[int, int]:
    """
    Get a summary of matches per page.
    
    Args:
        results: List of MatchResult objects
        
    Returns:
        Dictionary mapping page numbers to match counts
    """
    return {r.page_number: r.match_count for r in results}


def calculate_page_range(matched_pages: List[int], total_pages: int, padding: int = 2) -> List[int]:
    """
    Calculate final page list with padding and duplicate removal.
    
    Args:
        matched_pages: List of page numbers with matches (1-indexed)
        total_pages: Total number of pages in the PDF
        padding: Number of pages to include before/after each match
        
    Returns:
        Sorted list of unique page numbers to include
    """
    if not matched_pages:
        return []
    
    pages_to_include: Set[int] = set()
    
    for page in matched_pages:
        # Add the matched page and its padding window
        start = max(1, page - padding)
        end = min(total_pages, page + padding)
        
        for p in range(start, end + 1):
            pages_to_include.add(p)
    
    return sorted(pages_to_include)


def extract_pages(pdf_path: str, pages: List[int], output_path: str) -> str:
    """
    Extract specified pages from PDF and save to new file.
    
    Args:
        pdf_path: Path to source PDF
        pages: List of page numbers to extract (1-indexed)
        output_path: Path for output PDF
        
    Returns:
        Path to the created PDF file
    """
    if not pages:
        raise ValueError("No pages specified for extraction")
    
    reader = PdfReader(pdf_path)
    writer = PdfWriter()
    
    for page_num in pages:
        # Convert to 0-indexed
        writer.add_page(reader.pages[page_num - 1])
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else '.', exist_ok=True)
    
    with open(output_path, 'wb') as output_file:
        writer.write(output_file)
    
    return output_path


def get_total_pages(pdf_path: str) -> int:
    """Get total number of pages in a PDF."""
    doc = fitz.open(pdf_path)
    total = len(doc)
    doc.close()
    return total


def render_page_thumbnail(pdf_path: str, page_num: int, scale: float = 0.3) -> str:
    """
    Render a page as a thumbnail image.
    
    Args:
        pdf_path: Path to the PDF file
        page_num: Page number to render (1-indexed)
        scale: Scale factor for the thumbnail
        
    Returns:
        Base64-encoded PNG image
    """
    doc = fitz.open(pdf_path)
    
    try:
        page = doc[page_num - 1]  # Convert to 0-indexed
        
        # Create transformation matrix for scaling
        mat = fitz.Matrix(scale, scale)
        
        # Render page to pixmap
        pix = page.get_pixmap(matrix=mat)
        
        # Convert to PNG bytes
        img_bytes = pix.tobytes("png")
        
        # Encode to base64
        b64_image = base64.b64encode(img_bytes).decode('utf-8')
        
        return f"data:image/png;base64,{b64_image}"
    finally:
        doc.close()


def render_all_thumbnails(pdf_path: str, pages: List[int] = None, scale: float = 0.3) -> Dict[int, str]:
    """
    Render thumbnails for multiple pages.
    
    Args:
        pdf_path: Path to the PDF file
        pages: List of page numbers (1-indexed), or None for all pages
        scale: Scale factor for thumbnails
        
    Returns:
        Dictionary mapping page numbers to base64 images
    """
    doc = fitz.open(pdf_path)
    thumbnails = {}
    
    try:
        if pages is None:
            pages = list(range(1, len(doc) + 1))
        
        for page_num in pages:
            if 1 <= page_num <= len(doc):
                page = doc[page_num - 1]
                mat = fitz.Matrix(scale, scale)
                pix = page.get_pixmap(matrix=mat)
                img_bytes = pix.tobytes("png")
                b64_image = base64.b64encode(img_bytes).decode('utf-8')
                thumbnails[page_num] = f"data:image/png;base64,{b64_image}"
    finally:
        doc.close()
    
    return thumbnails


if __name__ == "__main__":
    # Simple test/demo
    import sys
    
    if len(sys.argv) < 3:
        print("Usage: python pdf_search.py <pdf_path> <keyword1> [keyword2] ...")
        sys.exit(1)
    
    pdf_file = sys.argv[1]
    keywords_to_search = sys.argv[2:]
    
    print(f"Searching '{pdf_file}' for: {keywords_to_search}")
    
    matches = search_keywords(pdf_file, keywords_to_search)
    total = get_total_pages(pdf_file)
    
    print(f"\nTotal pages in PDF: {total}")
    print("\nMatches found:")
    
    for match in matches:
        print(f"  Page {match.page_number}: {match.match_count} matches ({', '.join(match.keywords_found)})")
    
    if matches:
        matched_page_nums = [m.page_number for m in matches]
        pages_with_padding = calculate_page_range(matched_page_nums, total, padding=2)
        
        print(f"\nPages to include (with ±2 padding): {pages_with_padding}")
        print(f"Total pages in output: {len(pages_with_padding)}")
