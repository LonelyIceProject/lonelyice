"""Check a Linux release's ELF dependencies before publishing: python3 linux-elf-smoke.py <release>."""
from pathlib import Path
import re
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
    reports.append(str(library.relative_to(release)) + '\n' + report.replace(str(release), '$RELEASE'))

playerbots = release / 'plugins/playerbots/server/linux-x64/libplayerbots.so'
if playerbots.is_file():
    dynamic = subprocess.run(['readelf', '-d', str(playerbots)], capture_output=True, text=True, check=True).stdout
    assert 'libboost_thread.so' in dynamic, 'playerbots does not declare its Boost.Thread runtime dependency'
    linked = subprocess.run(['ldd', '-r', str(playerbots)], capture_output=True, text=True, check=True)
    report = linked.stdout + linked.stderr
    # ldd does not load the host executable. Script RTTI references are valid only if the host exports them.
    symbols = subprocess.run(['readelf', '--wide', '--dyn-syms', str(release / 'LonelyIce')],
                             capture_output=True, text=True, check=True).stdout
    exported = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[0].endswith(':') and fields[6] != 'UND':
            exported.add(fields[7].split('@')[0])
    unresolved = set(re.findall(r'undefined symbol:\s*(\S+)', report)) - exported
    assert not unresolved and 'not found' not in report, (sorted(unresolved), report)

(release / 'runtime-dependencies.txt').write_text('\n'.join(reports), encoding='utf-8')
print(f'PASS: {len(libraries)} ELF binaries resolve; playerbots declares and resolves Boost.Thread')
