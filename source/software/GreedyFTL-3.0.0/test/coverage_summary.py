import pathlib
import subprocess
import sys

result = subprocess.run(
    [sys.argv[1], "--summary", sys.argv[2]],
    check=True,
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
)
pathlib.Path(sys.argv[3]).write_text(result.stdout)
sys.stdout.write(result.stdout)
