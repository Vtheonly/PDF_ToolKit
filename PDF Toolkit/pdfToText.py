from pdfminer.high_level import extract_text



def pdf_to_text(pdf_path):
    try:
        text = extract_text(pdf_path)
        return text
    except Exception as e:
        print(f"An error occurred: {e}")
        return None





if __name__ == "__main__":
    pdf_path = "text_sample.pdf"  
    sentences = pdf_to_text(pdf_path)
    if sentences:
        for sentence in sentences:
            print("-------")
            print(sentence)
        