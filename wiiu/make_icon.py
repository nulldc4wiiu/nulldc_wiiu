# usage (any Python with Pillow): python3 make_icon.py <banner.png> <icon.png>
# 128x128 Wii U icon from the Homebrew Channel banner (128x48: swirl at the left, "nullDC" text): the swirl,
# scaled up, above the text, on the banner's own dark blue
import sys
from PIL import Image
b = Image.open(sys.argv[1]).convert('RGBA')
w, h = b.size
bg = b.getpixel((w - 3, h // 2))
icon = Image.new('RGBA', (128, 128), bg)
swirl = b.crop((0, 0, 44, 48)).resize((80, 87), Image.LANCZOS)
icon.alpha_composite(swirl, ((128 - 80) // 2, 2))
text = b.crop((40, 8, 128, 40))
icon.alpha_composite(text, ((128 - text.width) // 2, 128 - text.height - 4))
icon.convert('RGB').save(sys.argv[2])
print(bg, icon.size)
