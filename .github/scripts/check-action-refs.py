"""Every action a workflow names resolves to a published tag or branch.

A release-only workflow is not run by pull-request CI, so an unresolvable
`uses:` reference is otherwise first reported by the release itself
(0.0.20: `actions/upload-artifact@v8` stopped the JetBrains submission).
Local actions and full commit SHAs are not looked up.
"""
from pathlib import Path
import re
import subprocess
import sys

USES = re.compile(r'^\s*(?:-\s*)?uses:\s*([^\s#]+)', re.MULTILINE)


def references(root):
    found = {}
    for path in sorted((root / '.github').rglob('*.y*ml')):
        for match in USES.finditer(path.read_text(encoding='utf-8')):
            ref = match.group(1).strip('\'"')
            if ref.startswith(('./', 'docker://')) or '@' not in ref:
                continue
            found.setdefault(ref, []).append(path.relative_to(root).as_posix())
    return found


def resolves(ref):
    action, version = ref.rsplit('@', 1)
    if re.fullmatch(r'[0-9a-f]{40}', version):
        return True
    repo = '/'.join(action.split('/')[:2])
    out = subprocess.run(
        ['git', 'ls-remote', '--tags', '--heads', f'https://github.com/{repo}', version],
        capture_output=True, text=True, timeout=60)
    return out.returncode == 0 and bool(out.stdout.strip())


def main():
    root = Path(__file__).resolve().parents[2]
    missing = [(ref, files) for ref, files in references(root).items() if not resolves(ref)]
    for ref, files in missing:
        print(f'FAIL {ref} does not resolve (named in {", ".join(sorted(set(files)))})')
    print(f'{len(references(root)) - len(missing)} action references resolve, {len(missing)} do not')
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
