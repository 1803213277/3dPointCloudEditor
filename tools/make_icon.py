# 生成「3D点云编辑器」的应用图标：深色圆角底 + 散落的点云点（无圆环/球体）。
# 输出：app.ico（多尺寸）与 Assets 下的各个 Logo。
import os
import random

from PIL import Image, ImageDraw, ImageFilter

S = 1024
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "App1")
ASSETS = os.path.join(OUT_DIR, "Assets")
os.makedirs(ASSETS, exist_ok=True)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


# ---------------------------------------------------------------- 背景（圆角方形）
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
top = (12, 26, 46)      # 深海军蓝
bottom = (9, 58, 76)    # 深青
grad = Image.new("RGB", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(S):
    gd.line([(0, y), (S, y)], fill=lerp(top, bottom, y / (S - 1)))

mask = Image.new("L", (S, S), 0)
md = ImageDraw.Draw(mask)
md.rounded_rectangle([0, 0, S - 1, S - 1], radius=232, fill=255)
img.paste(grad, (0, 0), mask)

# 顶部柔光
sheen = Image.new("RGBA", (S, S), (0, 0, 0, 0))
sd = ImageDraw.Draw(sheen)
sd.ellipse([-260, -420, S + 260, 420], fill=(120, 200, 255, 34))
sheen = sheen.filter(ImageFilter.GaussianBlur(120))
img.alpha_composite(Image.composite(sheen, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask))

# 边框
border = Image.new("RGBA", (S, S), (0, 0, 0, 0))
bd = ImageDraw.Draw(border)
bd.rounded_rectangle([3, 3, S - 4, S - 4], radius=230, outline=(70, 120, 160, 170), width=7)
img.alpha_composite(border)

# ---------------------------------------------------------------- 散落的点云
random.seed(23)
layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ld = ImageDraw.Draw(layer)

# 主群：沿左下 -> 右上的斜向带状分布（像一段扫描点云），外加零散离群点
points = []
for _ in range(72):
    t = random.random()
    bx = 0.20 + 0.60 * t
    by = 0.70 - 0.34 * t + random.gauss(0.0, 0.10)
    x = (bx + random.gauss(0.0, 0.075)) * S
    y = (by + random.gauss(0.0, 0.11)) * S
    points.append((x, y, "band"))

for _ in range(34):
    x = random.uniform(0.10, 0.90) * S
    y = random.uniform(0.10, 0.90) * S
    points.append((x, y, "scatter"))

for x, y, kind in points:
    # 颜色：左青绿 -> 右紫，少量偏白的高亮点
    t = max(0.0, min(1.0, (x / S - 0.15) / 0.7))
    base = lerp((70, 232, 205), (150, 110, 255), t)
    if random.random() < 0.12:
        base = (225, 245, 255)
    glow = random.uniform(0.75, 1.15)
    color = (
        min(255, int(base[0] * glow)),
        min(255, int(base[1] * glow)),
        min(255, int(base[2] * glow)),
        random.randint(200, 255),
    )
    # 尺寸差异明显：大点 26~64，小点 10~30
    if kind == "band" and random.random() < 0.35:
        rad = random.uniform(22, 46)
    elif kind == "band":
        rad = random.uniform(12, 26)
    else:
        rad = random.uniform(8, 20)
    ld.ellipse([x - rad, y - rad, x + rad, y + rad], fill=color)

# 轻微的光晕（不打散整体形状）
bloom = layer.filter(ImageFilter.GaussianBlur(18))
img.alpha_composite(Image.blend(Image.new("RGBA", (S, S), (0, 0, 0, 0)), bloom, 0.5))
img.alpha_composite(layer)

# ---------------------------------------------------------------- 输出
master = img
master.save(os.path.join(OUT_DIR, "app_icon_master.png"))

sizes = [16, 24, 32, 48, 64, 128, 256]
ico_images = [master.resize((s, s), Image.LANCZOS) for s in sizes]
ico_path = os.path.join(OUT_DIR, "app.ico")
ico_images[-1].save(ico_path, format="ICO", sizes=[(s, s) for s in sizes])


def save_square(name, size):
    master.resize((size, size), Image.LANCZOS).save(os.path.join(ASSETS, name))


save_square("Square44x44Logo.scale-200.png", 88)
save_square("Square44x44Logo.targetsize-24_altform-unplated.png", 24)
save_square("Square150x150Logo.scale-200.png", 300)
save_square("StoreLogo.png", 50)
save_square("LockScreenLogo.scale-200.png", 48)


def make_panel(width, height, name, title, subtitle):
    panel = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    bg = Image.new("RGB", (width, height))
    bgd = ImageDraw.Draw(bg)
    for y in range(height):
        bgd.line([(0, y), (width, y)], fill=lerp(top, bottom, y / max(1, height - 1)))
    panel.paste(bg, (0, 0))

    icon_size = int(min(height * 0.62, width * 0.28))
    icon = master.resize((icon_size, icon_size), Image.LANCZOS)
    icon_x = int(width * 0.08)
    icon_y = (height - icon_size) // 2
    panel.alpha_composite(icon, (icon_x, icon_y))

    from PIL import ImageFont

    try:
        font_big = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", int(height * 0.18))
        font_small = ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf", int(height * 0.10))
    except Exception:
        font_big = font_small = None
    td = ImageDraw.Draw(panel)
    tx = icon_x + icon_size + int(width * 0.04)
    if font_big:
        td.text((tx, height * 0.30), title, font=font_big, fill=(235, 246, 255, 255))
    if font_small:
        td.text((tx, height * 0.54), subtitle, font=font_small, fill=(150, 220, 235, 255))

    panel.save(os.path.join(ASSETS, name))


make_panel(620, 300, "Wide310x150Logo.scale-200.png", "3D点云编辑器", "3D Point Cloud Editor")
make_panel(1240, 600, "SplashScreen.scale-200.png", "3D点云编辑器", "3D Point Cloud Editor")

print("icon written:", ico_path)
print("master:", os.path.join(OUT_DIR, "app_icon_master.png"))
