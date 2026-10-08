from PyPDF2 import PdfReader, PdfWriter

def create_class_diagram_bible(input_pdf, output_pdf):
    try:
        reader = PdfReader(input_pdf)
        writer = PdfWriter()
    except FileNotFoundError:
        print(f"Error: The file '{input_pdf}' was not found. Please check the filename.")
        return

    # PDF PAGE MAPPING LOGIC:
    # These ranges use 1-BASED PDF page numbers (what you see in a PDF viewer).
    # These correspond to the content found in the OCR analysis.
    
    extraction_plan = [
        # --- 1. THE FOUNDATION (Chapter 1) ---
        # Physical Pages 6-9 | PDF Pages 19-22
        # Content: Defines Class, Object, Encapsulation, Inheritance, Polymorphism.
        (19, 22),

        # --- 2. THE BIG PICTURE (Chapter 2) ---
        # Physical Pages 16-18 | PDF Pages 29-31
        # Content: Defines "Structure Diagrams" taxonomy.
        (29, 31),

        # --- 3. THE CORE BIBLE (Chapter 4) ---
        # Physical Pages 49-84 | PDF Pages 59-94
        # Content: The complete syntax (Attributes, Operations, Assoc, Aggreg, Comp, Gen, Interfaces).
        (59, 94),

        # --- 4. HIDDEN GEM: STATE TO CLASS MAPPING (Chapter 5) ---
        # Physical Page 86 | PDF Page 96
        # Content: Figure 5.2 - explicitly shows mapping State Machines to Class Attributes.
        (96, 96),

        # --- 5. INTEGRATION WITH SEQUENCE DIAGRAMS (Chapter 6) ---
        # Physical Pages 133-135 | PDF Pages 143-145
        # Content: "The Connection between a Class Diagram and a Sequence Diagram".
        (143, 145),

        # --- 6. REAL WORLD APPLICATION (Chapter 8) ---
        # Physical Pages 171-182 | PDF Pages 181-192
        # Content: Submission System & Stack examples (Fig 8.8, Fig 8.13).
        (181, 192),

        # --- 7. ADVANCED & METAMODELING (Chapter 9) ---
        # Physical Pages 186-192 | PDF Pages 196-202
        # Content: Metamodels (Class diagrams of UML) & Stereotypes.
        (196, 202),

        # --- 8. THE INDEX ---
        # Physical Pages 199-206 | PDF Pages 208-215
        # Content: Index for looking up syntax terms.
        (208, 215)
    ]

    print(f"Analyzing {input_pdf}...")
    total_pages = len(reader.pages)
    extracted_count = 0

    for start_page, end_page in extraction_plan:
        # Convert 1-based PDF numbers to 0-based Python index
        start_idx = start_page - 1
        end_idx = end_page 
        
        # Safety check
        if start_idx < 0: start_idx = 0
        if end_idx > total_pages: end_idx = total_pages

        print(f"Extracting PDF pages {start_page} to {end_page}...")
        
        for i in range(start_idx, end_idx):
            writer.add_page(reader.pages[i])
            extracted_count += 1

    with open(output_pdf, "wb") as f:
        writer.write(f)

    print(f"\nCOMPLETED. Created '{output_pdf}' with {extracted_count} pages.")

# --- RUN THE SCRIPT ---
# Ensure your PDF file name matches exactly what is on your disk
create_class_diagram_bible("UML_at_Classroom.pdf", "The_Class_Diagram_Complete_Collection.pdf")