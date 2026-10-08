#!/usr/bin/env python3
import argparse
import os
import re
import sys
from pypdf import PdfWriter

def main():
    parser = argparse.ArgumentParser(description="Merge PDF files in a folder based on numeric prefix.")
    parser.add_argument("folder_path", help="Path to the folder containing PDF files")
    parser.add_argument("--output", default="merged.pdf", help="Output filename (default: merged.pdf)")
    
    args = parser.parse_args()
    
    folder_path = args.folder_path
    output_filename = args.output
    
    # Handle "." or relative paths
    folder_path = os.path.abspath(folder_path)
    
    if not os.path.isdir(folder_path):
        print(f"Error: The directory '{folder_path}' does not exist.")
        sys.exit(1)
        
    print(f"Scanning directory: {folder_path}")
    
    # Regex to match files starting with a number followed by an underscore
    # Captures the number in group 1
    pattern = re.compile(r"^(\d+)_(.*)\.pdf$")
    
    files_to_merge = []
    
    # List files in the directory (non-recursive)
    try:
        all_files = os.listdir(folder_path)
    except OSError as e:
        print(f"Error accessing directory: {e}")
        sys.exit(1)
        
    for filename in all_files:
        filepath = os.path.join(folder_path, filename)
        
        # Skip directories and non-files
        if not os.path.isfile(filepath):
            continue
            
        match = pattern.match(filename)
        if match:
            # Extract the number for sorting
            order_num = int(match.group(1))
            files_to_merge.append((order_num, filepath, filename))
        else:
            # check if it's a pdf to notify user strictly about ignored pdfs
            if filename.lower().endswith('.pdf') and filename != output_filename:
                # Optional: Verbose log for ignored files
                # print(f"Ignored: {filename} (Does not match pattern Num_*.pdf)")
                pass

    if not files_to_merge:
        print("No matching PDF files found to merge.")
        print("Files must start with a number followed by an underscore (e.g., 1_intro.pdf).")
        sys.exit(0)
        
    # Sort by the extracted number
    files_to_merge.sort(key=lambda x: x[0])
    
    print(f"Found {len(files_to_merge)} files to merge:")
    for num, _, name in files_to_merge:
        print(f"  [{num}] {name}")
        
    output_path = os.path.join(folder_path, output_filename)
    
    try:
        merger = PdfWriter()
        
        for _, filepath, _ in files_to_merge:
            merger.append(filepath)
            
        merger.write(output_path)
        merger.close()
        
        print(f"\nSuccessfully created: {output_path}")
        
    except Exception as e:
        print(f"An error occurred during merging: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
