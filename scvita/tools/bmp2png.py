# bmp2png.py — convert the harness's 8-bit BMP captures to PNG.
import sys, glob, os
from PIL import Image
for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.bmp"))):
    Image.open(p).convert("RGB").save(p[:-4] + ".png")
