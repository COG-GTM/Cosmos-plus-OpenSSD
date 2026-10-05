import os
import pathlib
import sys

records = []
source = pathlib.Path(sys.argv[1]).read_text().splitlines()
current = None
hit = total = 0
for line in source:
    if line.startswith("SF:"):
        if current is not None:
            records.append((current, hit, total))
        current, hit, total = line[3:], 0, 0
    elif line.startswith("DA:"):
        _, count, *_ = line[3:].split(",")
        total += 1
        hit += int(count) > 0
if current is not None:
    records.append((current, hit, total))

root = os.path.commonpath([name for name, _, _ in records]) if len(records) > 1 else ""
output = ["| File | Lines hit / total | Coverage |", "|---|---:|---:|"]
for name, hit, total in records:
    label = os.path.relpath(name, root) if root else os.path.basename(name)
    output.append(f"| `{label}` | {hit} / {total} | {100 * hit / total if total else 0:.1f}% |")
pathlib.Path(sys.argv[2]).write_text("\n".join(output) + "\n")
