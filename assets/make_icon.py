#!/usr/bin/env python3
from PIL import Image,ImageDraw,ImageFilter
from pathlib import Path
import sys
p=Path(sys.argv[1]);p.parent.mkdir(parents=True,exist_ok=True)
im=Image.new('RGBA',(512,512),(3,10,28,255))
g=Image.new('RGBA',(512,512));d=ImageDraw.Draw(g)
d.rounded_rectangle((128,162,384,405),radius=37,outline=(255,28,69),width=27)
g=g.filter(ImageFilter.GaussianBlur(29))
im=Image.alpha_composite(im,g);d=ImageDraw.Draw(im)
d.rounded_rectangle((128,162,384,405),radius=37,fill=(80,6,33),outline=(255,91,119),width=11)
d.rounded_rectangle((103,137,409,175),radius=12,fill=(255,28,69))
d.rounded_rectangle((209,102,303,146),radius=14,outline=(255,87,115),width=11)
for x in (184,247,310):d.rounded_rectangle((x,207,x+17,357),radius=8,fill=(255,95,120))
im.convert('RGB').save(p)
