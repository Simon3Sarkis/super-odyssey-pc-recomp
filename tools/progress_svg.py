#!/usr/bin/env python3
"""Progress bars for the README, from the table in docs/STATUS.md.

docs/STATUS.md holds one table between `<!-- progress:begin -->` and `<!-- progress:end -->`, one row per area:

    | Area | Weight | Done |
    |---|---|---|
    | Core game | 30% | 80% |

This writes docs/media/progress/area-<slug>.svg for every row and docs/media/progress/overall.svg (the weighted
average). Run it after changing a number: `python3 tools/progress_svg.py`. Standard library only.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
STATUS = ROOT / "docs" / "STATUS.md"
OUT = ROOT / "docs" / "media" / "progress"

DONE, REST, TEXT = "#3bc45a", "#3b3f45", "#ffffff"  # the banner's green, a dark grey track, white text


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def bar(label, pct, width):
    pct = max(0, min(100, pct))
    fill = round(width * pct / 100)
    text = f"{pct}% {label}".strip()
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="28" viewBox="0 0 {width} 28" role="img" '
            f'aria-label="{text}"><rect width="{width}" height="28" rx="4" fill="{REST}"/>'
            f'<rect width="{fill}" height="28" rx="4" fill="{DONE}"/>'
            f'<text x="{width // 2}" y="18" text-anchor="middle" font-family="Verdana,DejaVu Sans,sans-serif" '
            f'font-size="13" fill="{TEXT}">{text}</text></svg>\n')


def rows(text):
    m = re.search(r"<!-- progress:begin -->(.*?)<!-- progress:end -->", text, re.S)
    if not m:
        sys.exit("docs/STATUS.md has no progress table (<!-- progress:begin --> ... <!-- progress:end -->)")
    out = []
    for line in m.group(1).splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3 or not cells[1].endswith("%") or not cells[2].endswith("%"):
            continue  # header, separator or anything else
        out.append((cells[0], int(cells[1][:-1]), int(cells[2][:-1])))
    if not out:
        sys.exit("the progress table in docs/STATUS.md has no rows")
    total = sum(w for _, w, _ in out)
    if total != 100:
        sys.exit(f"the weights in docs/STATUS.md add up to {total}%, not 100%")
    return out


def main():
    areas = rows(STATUS.read_text(encoding="utf-8"))
    OUT.mkdir(parents=True, exist_ok=True)
    for name, _, done in areas:
        (OUT / f"area-{slug(name)}.svg").write_text(bar("", done, 240), encoding="utf-8")
    overall = round(sum(w * d for _, w, d in areas) / 100)
    (OUT / "overall.svg").write_text(bar("overall", overall, 420), encoding="utf-8")
    for name, weight, done in areas:
        print(f"{name:24} weight {weight:3}%  done {done:3}%")
    print(f"{'overall':24}              {overall:3}%")


if __name__ == "__main__":
    main()
