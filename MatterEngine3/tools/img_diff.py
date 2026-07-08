#!/usr/bin/env python3
"""img_diff.py A.png B.png [--max-diff-pct 0.5] — exit 0 iff images match."""
import sys, argparse

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--max-diff-pct", type=float, default=0.5)
    ap.add_argument("--channel-tol", type=int, default=2)
    args = ap.parse_args()
    try:
        from PIL import Image
    except ImportError:
        sys.exit("img_diff: pip3 install --user pillow")
    ia, ib = Image.open(args.a).convert("RGB"), Image.open(args.b).convert("RGB")
    if ia.size != ib.size:
        print(f"DIFF size {ia.size} vs {ib.size}"); sys.exit(1)
    pa, pb = ia.tobytes(), ib.tobytes()
    npx = ia.size[0] * ia.size[1]
    bad = 0
    for i in range(0, len(pa), 3):
        if (abs(pa[i]-pb[i]) > args.channel_tol or
            abs(pa[i+1]-pb[i+1]) > args.channel_tol or
            abs(pa[i+2]-pb[i+2]) > args.channel_tol):
            bad += 1
    pct = 100.0 * bad / npx
    print(f"{'DIFF' if pct > args.max_diff_pct else 'MATCH'} "
          f"{bad}/{npx} px ({pct:.3f}%) exceed tol {args.channel_tol}")
    sys.exit(0 if pct <= args.max_diff_pct else 1)

if __name__ == "__main__":
    main()
