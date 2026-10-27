"""Check a Linux release's ELF dependencies before publishing: python3 linux-elf-smoke.py <release>."""
from pathlib import Path
import subprocess
import sys

release = Path(sys.argv[1]).resolve()
assert (release / 'LonelyIce').is_file(), 'Pass an unpacked Linux release directory'
libraries = [release / 'LonelyIce', *sorted(release.glob('lib*.so')),
             *sorted(release.glob('plugins/*/server/linux-x64/*.so'))]
reports = []
for library in libraries:
    result = subprocess.run(['ldd', str(library)], capture_output=True, text=True, check=True)
    report = result.stdout + result.stderr
    assert 'not found' not in report, f'{library}:\n{report}'
    reports.append(str(library.relative_to(release)) + '\n' + report)

playerbots = release / 'plugins/playerbots/server/linux-x64/libplayerbots.so'
if playerbots.is_file():
    dynamic = subprocess.run(['readelf', '-d', str(playerbots)], capture_output=True, text=True, check=True).stdout
    assert 'libboost_thread.so' in dynamic, 'playerbots does not declare its Boost.Thread runtime dependency'
    linked = subprocess.run(['ldd', '-r', str(playerbots)], capture_output=True, text=True, check=True)
    report = linked.stdout + linked.stderr
    assert 'undefined symbol' not in report and 'not found' not in report, report

(release / 'runtime-dependencies.txt').write_text('\n'.join(reports), encoding='utf-8')
print(f'PASS: {len(libraries)} ELF binaries resolve; playerbots declares and resolves Boost.Thread')
