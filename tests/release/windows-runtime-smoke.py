"""Verify a staged Windows release without development directories on PATH.

Usage: python windows-runtime-smoke.py <release directory> <dumpbin.exe>
"""
import ctypes
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

release = Path(sys.argv[1]).resolve()
dumpbin = Path(sys.argv[2]).resolve()
assert os.name == "nt", "This check runs on Windows"
ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
system = Path(os.environ["SystemRoot"]) / "System32"
files = list(release.rglob("*.dll")) + list(release.glob("*.exe"))
bundled = {p.name.lower() for p in files}
assert "zlib1.dll" not in bundled, "zlib must be linked statically"
for binary in files:
    output = subprocess.check_output([str(dumpbin), "/DEPENDENTS", str(binary)], text=True)
    for name in re.findall(r"^\s*([\w.-]+\.dll)\s*$", output, re.MULTILINE | re.IGNORECASE):
        name = name.lower()
        assert name != "zlib1.dll", f"{binary}: dynamic zlib dependency"
        if name.startswith(("api-ms-", "ext-ms-")):
            continue
        if name.startswith(("msvcp", "vcruntime", "concrt")):
            assert name in bundled, f"{binary}: MSVC runtime missing: {name}"
        else:
            assert name in bundled or (system / name).is_file(), f"{binary}: missing dependency: {name}"

env = dict(os.environ, PATH=str(system))
with tempfile.TemporaryDirectory(prefix="lonelyice-runtime-check-") as scratch:
    for args in (["--help"], ["--headless", "--help"], ["--tui", "--help"],
                 ["--pkg", "sync", "--dry-run", "--settings", str(release / "server.yaml")]):
        result = subprocess.run([str(release / "LonelyIce.exe"), *args], cwd=scratch,
                                env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (args, result.returncode, result.stdout, result.stderr)
print(f"PASS: {len(files)} binaries, static zlib, bundled runtime and clean-PATH launch: {release.name}")
