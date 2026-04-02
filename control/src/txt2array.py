# convert_to_c.py

import sys
import os

if len(sys.argv) < 3:
    print("Usage: python convert_to_c.py input.txt output.h")
    sys.exit(1)

input_path = sys.argv[1]
output_path = sys.argv[2]

# Read raw bytes
with open(input_path, "rb") as f:
    data = f.read()

# Write UTF-8 file
with open(output_path, "w", encoding="utf-8") as out:
    out.write("#ifndef ONE_MONTH_ARR_H\n")
    out.write("#define ONE_MONTH_ARR_H\n\n")
    out.write("const unsigned char one_month_txt[] = {\n")

    for i, b in enumerate(data):
        out.write(f"    0x{b:02X},")
        if (i + 1) % 16 == 0:
            out.write("\n")

    out.write("\n};\n")
    out.write(f"const unsigned int one_month_txt_len = {len(data)};\n\n")
    out.write("#endif\n")

print(f"Generated UTF-8 header file: {output_path}")