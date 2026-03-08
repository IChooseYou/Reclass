"""
Pixel boundary test: validates no Fusion outline leak at the workspace→editor seam.

Usage:
  python tests/test_pixels.py [screenshot.png]

If no screenshot given, launches Reclass.exe --screenshot to grab one.
Scans for the specific Fusion outline artifact: color (23,23,23) which is
window.darker(140) for the VS2022 Dark theme background #1e1e1e.
"""
import sys, os, subprocess
from PIL import Image
from collections import defaultdict

GRAB_PATH = os.path.join("build", "test_grab.png")

def get_screenshot(path):
    if not os.path.exists(path):
        print(f"Launching Reclass.exe --screenshot {path}")
        subprocess.run(["./build/Reclass.exe", "--screenshot", path],
                       timeout=15, check=True)
    return Image.open(path)

def scan_for_artifact(img):
    """Scan entire image for the Fusion outline color (23,23,23).
    Also find all near-black pixels (< 28,28,28) that aren't the
    theme background (30,30,30)."""
    w, h = img.size
    px = img.load()

    target = (23, 23, 23)
    bg = (30, 30, 30)

    target_hits = []
    dark_hits = defaultdict(list)  # color → [(x,y), ...]

    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y][:3]
            if r == target[0] and g == target[1] and b == target[2]:
                target_hits.append((x, y))
            elif r < 28 and g < 28 and b < 28 and (r, g, b) != (0, 0, 0):
                # Near-black but not pure black (text anti-aliasing) and not bg
                dark_hits[(r, g, b)].append((x, y))

    return target_hits, dark_hits

def summarize_region(hits):
    """Summarize a list of (x,y) hits."""
    if not hits:
        return "none"
    xs = [p[0] for p in hits]
    ys = [p[1] for p in hits]
    return (f"{len(hits)}px  x=[{min(xs)}..{max(xs)}] y=[{min(ys)}..{max(ys)}]  "
            f"size={max(xs)-min(xs)+1}x{max(ys)-min(ys)+1}")

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else GRAB_PATH
    img = get_screenshot(path)
    w, h = img.size
    print(f"Image: {w}x{h}")

    target_hits, dark_hits = scan_for_artifact(img)

    print(f"\n(23,23,23) Fusion outline pixels: {summarize_region(target_hits)}")

    if dark_hits:
        print(f"\nOther near-black pixels (< 28,28,28):")
        for c, positions in sorted(dark_hits.items(), key=lambda t: -len(t[1])):
            print(f"  ({c[0]:3},{c[1]:3},{c[2]:3}): {summarize_region(positions)}")

    if target_hits:
        # Show row distribution (condensed)
        rows = defaultdict(list)
        for x, y in target_hits:
            rows[y].append(x)
        print(f"\n(23,23,23) row detail:")
        for y in sorted(rows.keys()):
            xs = sorted(rows[y])
            if len(xs) > 5:
                print(f"  y={y}: {len(xs)}px  x=[{xs[0]}..{xs[-1]}]")
            else:
                print(f"  y={y}: {len(xs)}px  x={xs}")

        print(f"\nFAIL: Found {len(target_hits)} Fusion outline pixels (23,23,23)")
        sys.exit(1)
    else:
        print("\nPASS: No Fusion outline artifact found")
        sys.exit(0)

if __name__ == "__main__":
    main()
