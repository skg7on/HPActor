from math import cos, pi, sin
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "hpactor-logo.png"
PREVIEW = ROOT / "assets" / "hpactor-logo-preview.png"

SCALE = 4
WIDTH = 1800
HEIGHT = 620


def c(hex_color, alpha=255):
    hex_color = hex_color.lstrip("#")
    return tuple(int(hex_color[i : i + 2], 16) for i in (0, 2, 4)) + (alpha,)


def xy(point):
    return tuple(int(round(v * SCALE)) for v in point)


def box(rect):
    return tuple(int(round(v * SCALE)) for v in rect)


def scaled(points):
    return [xy(point) for point in points]


def font(path, size):
    return ImageFont.truetype(path, size * SCALE)


def regular_polygon(cx, cy, radius, sides=6, rotation=-pi / 6):
    return [
        (
            cx + radius * cos(rotation + 2 * pi * i / sides),
            cy + radius * sin(rotation + 2 * pi * i / sides),
        )
        for i in range(sides)
    ]


def alpha_composite_masked(base, fill, mask):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    layer.paste(fill, (0, 0), mask)
    base.alpha_composite(layer)


def make_linear_gradient(size, start_color, end_color):
    width, height = size
    gradient = Image.new("RGBA", size)
    pixels = gradient.load()
    for y in range(height):
        t = y / max(1, height - 1)
        row = tuple(
            int(start_color[i] * (1 - t) + end_color[i] * t) for i in range(4)
        )
        for x in range(width):
            pixels[x, y] = row
    return gradient


def text_width(draw, position, text, font_obj):
    bbox = draw.textbbox(position, text, font=font_obj)
    return bbox[2] - bbox[0]


def text_y_for_visible_top(draw, text, font_obj, visible_top):
    bbox = draw.textbbox((0, 0), text, font=font_obj)
    return visible_top - bbox[1] / SCALE


def text_y_for_visible_bottom(draw, text, font_obj, visible_bottom):
    bbox = draw.textbbox((0, 0), text, font=font_obj)
    return visible_bottom - bbox[3] / SCALE


def draw_node(draw, center, radius, fill, outline, width=4):
    draw.ellipse(box((center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius)), fill=fill)
    draw.ellipse(
        box((center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius)),
        outline=outline,
        width=width * SCALE,
    )


def main():
    canvas = Image.new("RGBA", (WIDTH * SCALE, HEIGHT * SCALE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)

    center = (290, 310)
    outer = regular_polygon(center[0], center[1], 206)
    inner = regular_polygon(center[0], center[1], 154)

    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    shadow_draw = ImageDraw.Draw(shadow)
    shadow_draw.polygon(scaled([(x + 14, y + 18) for x, y in outer]), fill=c("#05111E", 70))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18 * SCALE)))

    mask = Image.new("L", canvas.size, 0)
    mask_draw = ImageDraw.Draw(mask)
    mask_draw.polygon(scaled(outer), fill=255)
    gradient = make_linear_gradient(canvas.size, c("#10233E"), c("#07121F"))
    alpha_composite_masked(canvas, gradient, mask)

    draw.polygon(scaled(outer), outline=c("#16D9F7"), width=8 * SCALE)
    draw.line(scaled(outer + [outer[0]]), fill=c("#75F6B3", 150), width=2 * SCALE)

    for r, alpha in ((226, 34), (244, 18)):
        glow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
        glow_draw = ImageDraw.Draw(glow)
        glow_draw.polygon(scaled(regular_polygon(center[0], center[1], r)), outline=c("#18D5FF", alpha), width=10 * SCALE)
        canvas.alpha_composite(glow.filter(ImageFilter.GaussianBlur(8 * SCALE)))

    actor_nodes = [
        (center[0], center[1] - 142),
        (center[0] + 124, center[1] - 72),
        (center[0] + 124, center[1] + 72),
        (center[0], center[1] + 142),
        (center[0] - 124, center[1] + 72),
        (center[0] - 124, center[1] - 72),
    ]

    for i, node in enumerate(actor_nodes):
        next_node = actor_nodes[(i + 1) % len(actor_nodes)]
        draw.line(scaled([node, next_node]), fill=c("#22C7F2", 75), width=5 * SCALE)
        draw.line(scaled([node, center]), fill=c("#8AF7C2", 82), width=4 * SCALE)

    draw.polygon(scaled(inner), outline=c("#2C4966", 230), width=5 * SCALE)

    core_radius = 58
    draw.ellipse(
        box(
            (
                center[0] - core_radius,
                center[1] - core_radius,
                center[0] + core_radius,
                center[1] + core_radius,
            )
        ),
        fill=c("#0A1727"),
        outline=c("#E7FBFF"),
        width=5 * SCALE,
    )

    draw.line(scaled([(center[0] - 26, center[1] + 34), (center[0], center[1] - 38), (center[0] + 29, center[1] + 34)]), fill=c("#15D6F7"), width=13 * SCALE, joint="curve")
    draw.line(scaled([(center[0] - 12, center[1] + 5), (center[0] + 16, center[1] + 5)]), fill=c("#8AF7C2"), width=10 * SCALE)

    node_palette = [
        (c("#0EE3FF"), c("#E7FBFF")),
        (c("#77F8B4"), c("#E7FBFF")),
        (c("#FFB45C"), c("#FFF1DE")),
        (c("#0EE3FF"), c("#E7FBFF")),
        (c("#77F8B4"), c("#E7FBFF")),
        (c("#FFB45C"), c("#FFF1DE")),
    ]
    for node, colors in zip(actor_nodes, node_palette):
        draw_node(draw, node, 18, colors[0], colors[1], 4)

    for offset, alpha in ((0, 210), (22, 145), (44, 85)):
        y = center[1] - 42 + offset
        draw.line(scaled([(center[0] + 68, y), (center[0] + 164, y)]), fill=c("#14D9F7", alpha), width=7 * SCALE)
        draw.polygon(
            scaled(
                [
                    (center[0] + 164, y - 13),
                    (center[0] + 196, y),
                    (center[0] + 164, y + 13),
                ]
            ),
            fill=c("#14D9F7", alpha),
        )

    title_hp = font("/System/Library/Fonts/Supplemental/Arial Black.ttf", 156)
    title_actor = font("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 156)
    tagline = font("/System/Library/Fonts/Supplemental/Arial.ttf", 43)

    x0 = 580
    title_bottom = 320
    hp_y = text_y_for_visible_bottom(draw, "HP", title_hp, title_bottom)
    actor_y = text_y_for_visible_bottom(draw, "Actor", title_actor, title_bottom)
    draw.text(xy((x0 + 3, hp_y + 5)), "HP", font=title_hp, fill=c("#06111D", 45))
    draw.text(xy((x0, hp_y)), "HP", font=title_hp, fill=c("#0A1D31"))
    hp_width = text_width(draw, xy((x0, hp_y)), "HP", title_hp) / SCALE
    actor_x = x0 + hp_width + 14
    draw.text(xy((actor_x + 3, actor_y + 5)), "Actor", font=title_actor, fill=c("#06111D", 30))
    draw.text(xy((actor_x, actor_y)), "Actor", font=title_actor, fill=c("#223348"))

    underline_y = 371
    draw.rounded_rectangle(box((x0 + 3, underline_y, x0 + 273, underline_y + 13)), radius=7 * SCALE, fill=c("#16D9F7"))
    draw.rounded_rectangle(box((x0 + 287, underline_y, x0 + 430, underline_y + 13)), radius=7 * SCALE, fill=c("#82F5B6"))
    draw.rounded_rectangle(box((x0 + 444, underline_y, x0 + 498, underline_y + 13)), radius=7 * SCALE, fill=c("#FFB45C"))

    draw.text(xy((x0 + 2, 411)), "High-performance distributed Actor runtime", font=tagline, fill=c("#536A7F"))
    draw.text(xy((x0 + 2, 464)), "for stateful AI inference services", font=tagline, fill=c("#536A7F"))

    canvas = canvas.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(OUT)

    preview = Image.new("RGBA", canvas.size, c("#F7FBFF"))
    preview.alpha_composite(canvas)
    preview.save(PREVIEW)

    print(OUT)
    print(PREVIEW)


if __name__ == "__main__":
    main()
