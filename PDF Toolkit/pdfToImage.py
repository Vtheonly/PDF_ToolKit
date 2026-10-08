from pdf2image import convert_from_path
import os

output_folder = "extracted_images"
if not os.path.exists(output_folder):
    os.makedirs(output_folder)

pdf_path = "xx.pdf"
images = convert_from_path(pdf_path, dpi=300)  #  dpi for quality

# Save each page as an image
for i, image in enumerate(images, start=1):
    image.save(f"{output_folder}/page_{i}.png", "PNG")
print("PDF successfully converted to images!")