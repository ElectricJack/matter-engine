"""Snapshot the two bounded native OBJ exports; never update legacy goldens."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True, help="Use a new directory when reproducing")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
records = []
for name in ["CastlePavingSlab", "AlpineFlower"]:
    directory = repo / "MatterEngine3/tests/sandbox/obj_export_golden" / name
    copied = []
    for suffix in [".obj", ".mtl", "_albedo.png", "_normal.png", "_roughness.png", "_metallic.png", "_ao.png"]:
        source = directory / (name + suffix)
        if source.stat().st_size > 512*1024:
            raise ValueError("bounded capture limit exceeded: " + str(source))
        target = args.output / source.name
        shutil.copyfile(source, target)
        copied.append({"path": source.name, "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
    records.append({"module": name, "files": copied})
(args.output / "files.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
print(f"Captured {len(records)} bounded native exports in {args.output}")
