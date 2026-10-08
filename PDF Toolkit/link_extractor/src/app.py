import fitz  # PyMuPDF

def extract_links_from_pdf(pdf_path):
    try:
        # Open the PDF file
        doc = fitz.open(pdf_path)
        
        # List to store extracted links
        links = []
        
        # Iterate through each page
        for page_num in range(len(doc)):
            # Get the current page
            page = doc.load_page(page_num)
            
            # Extract links (which include hyperlinks)
            page_links = page.get_links()
            
            # Iterate through each link
            for link in page_links:
                # Check if the link is a URI (web link)
                if link['kind'] == fitz.LINK_URI:
                    # Extract the URI (link)
                    uri = link['uri']
                    
                    # Append the link to the links list
                    links.append(uri)
        
        # Close the document
        doc.close()
        
        return links
    except Exception as e:
        print(f"An error occurred: {e}")
        return []

# Example usage
pdf_path = r'C:\Users\merse\OneDrive\Documents\Learn\Courses\1 - Coding\Coding Books\AI\Neural Networks from Scratch in Python.pdf'
links = extract_links_from_pdf(pdf_path)
x =0
print("Extracted Links:")
for link in links:
    if(len(link)==19):
        # print(link)
        x+=1
    
    
print(x)