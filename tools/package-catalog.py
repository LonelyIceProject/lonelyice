"""Merge two built plugin trees and pack an official catalog with the launcher.

Usage: python tools/package-catalog.py --exe <LonelyIce> --windows <plugins>
       --linux <plugins> --sources <repositories> --out <catalog>
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
for name in ('exe', 'windows', 'linux', 'sources', 'out'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
exe, windows, linux, sources, out = (getattr(args, key).resolve() for key in
                                    ('exe', 'windows', 'linux', 'sources', 'out'))
out.mkdir(parents=True, exist_ok=True)
source_by_id = {}
for source in sources.iterdir():
    manifest = source / 'plugin.json'
    if manifest.is_file():
        source_by_id[json.loads(manifest.read_text(encoding='utf-8'))['id']] = source
entries, release_plan = [], []
with tempfile.TemporaryDirectory(prefix='lonelyice-catalog-') as temporary:
    stage_root = Path(temporary).resolve()
    for win in sorted(windows.iterdir()):
        if not (win / 'plugin.json').is_file():
            continue
        manifest = json.loads((win / 'plugin.json').read_text(encoding='utf-8'))
        plugin_id, version = manifest['id'], manifest['version']
        lin = linux / plugin_id
        other = json.loads((lin / 'plugin.json').read_text(encoding='utf-8'))
        # Common payload must match; only platform metadata and native libraries may differ.
        for metadata in (manifest, other):
            metadata.pop('platforms', None)
        assert manifest == other, f'{plugin_id}: platform manifests differ'
        source = source_by_id[plugin_id]
        assert json.loads((source / 'plugin.json').read_text(encoding='utf-8'))['version'] == version
        assert not subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain']), source
        sha = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        remote = subprocess.check_output(['git', '-C', str(source), 'remote', 'get-url', 'origin'], text=True).strip()
        repo = remote.removesuffix('.git').replace('git@github.com:', '').removeprefix('https://github.com/')
        assert repo.startswith('LonelyIceProject/'), repo
        staged = stage_root / plugin_id
        assert staged.is_relative_to(stage_root)
        shutil.copytree(win, staged)
        shutil.copytree(lin / 'server/linux-x64', staged / 'server/linux-x64')
        for file in lin.rglob('*'):
            rel = file.relative_to(lin)
            if not file.is_file() or rel.parts[0] == 'server' or rel.name == 'plugin.json':
                continue
            assert (win / rel).is_file(), (plugin_id, rel)
            if file.read_bytes() != (win / rel).read_bytes():
                # Git can check the same upstream text out with CRLF on Windows.
                assert file.read_text(encoding='utf-8') == (win / rel).read_text(encoding='utf-8'), (plugin_id, rel)
        manifest['platforms'] = ['windows-x64', 'linux-x64']
        (staged / 'plugin.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        subprocess.run([str(exe), '--pkg', 'pack', str(staged), str(out)], check=True, capture_output=True)
        filename = f'{plugin_id}-{version}.zip'
        archive = out / filename
        with zipfile.ZipFile(archive) as package:
            assert package.testzip() is None, archive
            assert not any(Path(n).suffix in {'.pdb', '.ilk', '.lib', '.exp', '.a'} for n in package.namelist())
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        download = f'https://github.com/{repo}/releases/download/v{version}/{filename}'
        entry = {key: manifest[key] for key in ('id', 'version', 'name', 'description', 'platforms', 'locales', 'depends', 'conflicts') if key in manifest}
        entry.update(core=manifest['core']['abi'], url=filename, sha256=digest, size=archive.stat().st_size,
                     page=f'https://lonelyice.org/packages/{plugin_id}', source_repo=f'https://github.com/{repo}', source_ref=sha,
                     download_url=download)
        if (out / f'{plugin_id}-{version}.png').exists():
            entry['icon'] = f'{plugin_id}-{version}.png'
        entries.append(entry)
        (out / (filename + '.sha256')).write_text(f'{digest}  {filename}\n', encoding='utf-8')
        release_plan.append(dict(id=plugin_id, version=version, repo=repo, sha=sha, filename=filename, url=download))
        print(f'{plugin_id} {version}: {archive.stat().st_size} bytes, Windows + Linux')
(out / 'index.json').write_text(json.dumps(dict(format=1, name={'en': 'LonelyIce catalog'}, packages=entries), ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
(out / 'release-plan.json').write_text(json.dumps(release_plan, indent=2) + '\n', encoding='utf-8')
