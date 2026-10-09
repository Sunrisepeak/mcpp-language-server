from pathlib import Path
import json,hashlib
floor=Path(__file__).parent.parent
assert json.loads((floor/'retry1-progress.json').read_text())['status']=='complete', 'clean72 build/test/package required; no measurement started'
assert json.loads((floor/'source-progress.json').read_text())['status']=='complete'
assert json.loads((floor/'portability.json').read_text())['problems']==[]
pkg=floor/'dist/clangd-23.1.0-mcppls.0-linux-x64/clangd'
meta=json.loads((pkg/'engine.json').read_text())
assert meta['patch-series-sha256']=='b53e91d831b3b45944d7bc2904145221497333dca2e890e75e7c5cfc3ed3b7df'
assert meta['llvm-tree-commit']=='82cdc7777e9dec4712b5913a8cfd31ea3634b99b'
assert meta['fork-commit']=='c30429fbe876c10b7dbe372a42e2d4b867b5970c'
assert meta['llvm-commit']=='ea7d852a70e8bdfaf601d6626a760f9771b2c4b4'
assert meta['platform']=='linux-x64'
assert hashlib.sha256((pkg/'bin/clangd').read_bytes()).hexdigest()==meta['sha256']
resource=pkg/'lib/clang/23/include'
headers={str(p.relative_to(resource)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(resource.rglob('*')) if p.is_file()}
assert len(headers)>=300
identity={'scope':'Final72 actual package identity before qualification; not a performance result','engine_metadata':meta,'resource_headers':headers}
target=Path(__file__).parent/'final-package-identity.json'
if target.exists():assert json.loads(target.read_text())==identity
else:target.write_text(json.dumps(identity,indent=2)+'\n')
