"""Real Unicode filenames, environment and CLI argv on Windows and POSIX."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

helper, extract = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="misrc-path-") as temporary:
    root = Path(temporary)
    directory = root / "「・ 」中文📼 & '"
    directory.mkdir()
    env = os.environ.copy()
    env["MISRC_PATH_TEST_ENV"] = str(directory)
    env["MISRC_PATH_TEST_EMPTY"] = ""
    env.pop("MISRC_PATH_TEST_ABSENT", None)
    subprocess.run([helper, str(directory)], env=env, check=True)
    assert (directory / "中文📼.bin").read_bytes() == b"path-bytes"
    assert (directory / "目录").is_dir()
    source = root / "ascii.raw"
    source.write_bytes(bytes(range(128)))
    reference = root / "reference.raw"
    subprocess.run([extract, "-i", str(source), "-a", str(reference)], check=True)
    names = ["「・ 」中文📼.raw"]
    if os.name != "nt":
        names += ['quote"name.raw', "literal\\backslash.raw", "line\nfeed.raw"]
    for name in names:
        incoming = directory / ("input-" + name)
        outgoing = directory / ("output-" + name)
        incoming.write_bytes(source.read_bytes())
        subprocess.run([extract, "-i", str(incoming), "-a", str(outgoing)], check=True)
        assert outgoing.read_bytes() == reference.read_bytes()
    pipe = subprocess.run([extract, "-i", "-", "-a", "-"], input=source.read_bytes(),
                          stdout=subprocess.PIPE, check=True)
    assert pipe.stdout == reference.read_bytes()
print("Unicode filesystem, environment, CLI input/output and stdin/stdout PASS")
