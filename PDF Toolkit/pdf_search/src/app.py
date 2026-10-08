working_directory = '/home/mersel/Documents/Learn/University/Semestre 5/1 - Compilation'  # Update this with the correct directory

import subprocess
from collections import defaultdict

# Define keywords, working directory, and output files

keywords = ['flex','lex','yacc','Flex','Lex']

output_file = 'urls.txt'
final_output_file = 'MipsOutput.txt'

# Construct the pdfgrep command
pdfgrep_command = ['pdfgrep', '-r'] + [f'-e {keyword}' for keyword in keywords]
command_str = ' '.join(pdfgrep_command)

# Print the command being run
print(f"Running command: {command_str} in directory {working_directory}")

# Run the pdfgrep command in the specified working directory and save the output to urls.txt
with open(output_file, 'w') as out:
    result = subprocess.run(command_str, shell=True, cwd=working_directory, stdout=out)

# Read the output from urls.txt
with open(output_file, 'r') as f:
    lines = f.readlines()

# Dictionary to store occurrences
occurrences = defaultdict(int)

# Process each line from the output
for line in lines:
    file_path = line.split(':', 1)[0]
    
    # Increase the count for each occurrence of the file
    occurrences[file_path] += 1

# Sort the occurrences dictionary by the occurrence count in descending order
sorted_occurrences = sorted(occurrences.items(), key=lambda item: item[1], reverse=True)

# Write the sorted file paths and their occurrences to Out.txt with indentation
with open(final_output_file, 'w') as out:
    out.write("Keywords : \t"+str(keywords) +"\n\n")
    for file_path, count in sorted_occurrences:
        out.write(f"{file_path}\n")  # Write the file path
        out.write(f"\t\tOccurrences: {count}\n")  # Write the occurrences with indentation



print(f"Sorted files and occurrences are saved in {final_output_file}.")
