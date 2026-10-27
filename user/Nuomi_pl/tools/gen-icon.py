#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# tools/gen-icon.py -- 生成 Nuomi 程序图标 nuomi.ico（纯 Pillow，零 AI 成本）
# 用法: python tools/gen-icon.py [输出.ico]   (默认输出 nuomi.ico)
# 设计: 圆角方块(靛→紫→粉 横向渐变) + 白色麻糬椭圆 + 小眼睛 + 粉腮红; 1024 超采样后 Lanczos 缩多尺寸。
import sys
from PIL import Image, ImageDraw

S = 1024  # 超采样画布边长
OUT = sys.argv[1] if len(sys.argv) > 1 else "nuomi.ico"

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

def grad_strip(width, stops):
    """1×width 横向渐变带; stops=[(pos0..1,color),...]"""
    img = Image.new("RGB", (width, 1))
    d = ImageDraw.Draw(img)
    for x in range(width):
        t = x / (width - 1)
        col = stops[-1][1]
        for i in range(len(stops) - 1):
            p0, c0 = stops[i]; p1, c1 = stops[i + 1]
            if p0 <= t <= p1:
                local = 0.0 if p1 == p0 else (t - p0) / (p1 - p0)
                col = lerp(c0, c1, local); break
        d.point((x, 0), col)
    return img

def main():
    c1 = (99, 102, 241)   # indigo  #6366F1
    c2 = (168, 85, 247)   # purple  #A855F7
    c3 = (236, 72, 153)   # pink    #EC4899
    # 背景: 横向三色渐变, 再与竖向渐变轻混, 得到斜向观感
    horiz = grad_strip(S, [(0.0, c1), (0.5, c2), (1.0, c3)]).resize((S, S), Image.BILINEAR)
    vert  = grad_strip(S, [(0.0, c3), (1.0, c1)])              # 1×S
    vert  = vert.transpose(Image.TRANSPOSE).resize((S, S), Image.BILINEAR)  # 竖成 S×S
    bg = Image.blend(horiz, vert, 0.30).convert("RGBA")

    # 圆角方块(四角透明)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
    bg.putalpha(mask)

    d = ImageDraw.Draw(bg)
    cx, cy = S // 2, int(S * 0.55)
    rw, rh = int(S * 0.30), int(S * 0.235)          # 麻糬身体(白色椭圆)
    # 底部柔和的浅阴影, 增加体积感
    d.ellipse([cx - rw, cy - rh + int(S*0.02), cx + rw, cy + rh + int(S*0.02)],
              fill=(0, 0, 0, 40))
    d.ellipse([cx - rw, cy - rh, cx + rw, cy + rh], fill=(255, 255, 255, 255))
    # 顶部柔光
    d.ellipse([cx - int(rw*0.55), cy - rh + int(S*0.03), cx + int(rw*0.1), cy - rh + int(S*0.16)],
              fill=(255, 255, 255, 255))

    eye_r = int(S * 0.036); ex = int(S * 0.085); ey = cy - int(S * 0.025)
    dark = (39, 39, 60, 255)
    d.ellipse([cx - ex - eye_r, ey - eye_r, cx - ex + eye_r, ey + eye_r], fill=dark)
    d.ellipse([cx + ex - eye_r, ey - eye_r, cx + ex + eye_r, ey + eye_r], fill=dark)

    brw, brh = int(S * 0.052), int(S * 0.030); bx = int(S * 0.13); by = cy + int(S * 0.055)
    blush = (250, 165, 195, 255)
    d.ellipse([cx - bx - brw, by - brh, cx - bx + brw, by + brh], fill=blush)
    d.ellipse([cx + bx - brw, by - brh, cx + bx + brw, by + brh], fill=blush)

    # 输出多尺寸 ICO
    base = bg.resize((256, 256), Image.LANCZOS)
    sizes = [(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)]
    # bitmap_format="bmp" -> 不用 PNG 压缩，兼容 Inno Setup 的图标编译器（它只认 BMP 图标）
    base.save(OUT, format="ICO", sizes=sizes, bitmap_format="bmp")
    print(f"wrote {OUT}")

if __name__ == "__main__":
    main()
