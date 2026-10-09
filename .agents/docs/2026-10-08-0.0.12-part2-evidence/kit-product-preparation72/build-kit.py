"""Prepare a same-source Linux kit only after clean72 and raw qualification.

Uses the immutable full72 LLVM tree, its actual clang producer, and independently
captured Ubuntu20 C/kernel headers. This is local kit provenance, not a shipped
payload, three-platform qualification or proof of host-free product installation.
"""
from pathlib import Path
import sys,os,json,hashlib,subprocess,shutil,time,signal
root=Path(__file__).parent;floor=root.parent
sys.path.insert(0,str(floor/'qualification'))
import final72_identity
q=floor/'qualification'
assert json.loads((q/'matrix-distribution-audit.json').read_text())['all_per_context_requirements_passed']
for name in ['native-settled-distribution','four-arm']:
    assert json.loads((q/name/'summary.json').read_text())['all_explicit_performance_checks_pass']
assert json.loads((root/'sysroot-progress.json').read_text())['status']=='complete'
assert not(root/'identity.json').exists(), 'refuse to overwrite kit attempt'
assert not(root/'kit').exists() and not(root/'work').exists() and not(root/'producer').exists()
assert shutil.disk_usage(root).free>=2*1024**3
for path in Path('/proc').iterdir():
    if not path.name.isdigit():continue
    try:name=(path/'comm').read_text().strip()
    except FileNotFoundError:continue
    if name.startswith(('clangd','clang','gcc','g++','cc1')) or name in {'ninja','rustc'}:
        raise SystemExit('compiler/engine activity blocks sequential kit generation')
source=Path('/dev/shm/mcppls-part2-floor72-source')
assert subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()=='82cdc7777e9dec4712b5913a8cfd31ea3634b99b'
subprocess.run(['git','-C',str(source),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
headers=json.loads((root/'sysroot-progress.json').read_text())['headers']
assert all(hashlib.sha256((root/'sysroot'/name).read_bytes()).hexdigest()==digest for name,digest in headers.items())
producer=root/'producer';(producer/'bin').mkdir(parents=True)
clang=producer/'bin/clang';shutil.copy2(Path('/dev/shm/mcppls-part2-floor72-build/bin/clang').resolve(),clang)
(producer/'bin/clang++').symlink_to('clang')
pkg=floor/'dist/clangd-23.1.0-mcppls.0-linux-x64/clangd'
shutil.copytree(pkg/'lib/clang/23/include',producer/'lib/clang/23/include')
work=root/'work';work.mkdir();(work/'llvm-project').symlink_to(source,target_is_directory=True)
tool=Path('/home/speak/workspace/github/mcpp-language-server/target/x86_64-linux-gnu/c6a96775022a4395/bin/mcppls-devtools/mcppls-devtools')
archive=Path('/home/speak/workspace/github/mcppls-clangd/llvm-src.tar.gz')
args=[str(tool),'kit','--platform','linux-x64','--out',str(root/'kit'),'--work',str(work),'--cache',str(root/'offline-cache'),'--source',str(archive),'--jobs','6','--sysroot-include',str(root/'sysroot/usr/include')]
for name in ['libc6-dev-copyright.txt','linux-libc-dev-copyright.txt']:
    args+=['--sysroot-license',str(root/'sysroot-licenses'/name)]
env=os.environ.copy();env.update(CC=str(clang),CXX=str(producer/'bin/clang++'),CMAKE='/usr/bin/cmake',NINJA='/usr/bin/ninja',CFLAGS='',CXXFLAGS='',CPPFLAGS='',LDFLAGS='')
identity={'status':'running','scope':'Local full72-source/producer Linux kit with Ubuntu20 C/kernel headers. Not an immutable assembled payload or floor20 full product proof.','started_unix':time.time(),'command':args,'environment_overrides':{name:env[name] for name in ['CC','CXX','CMAKE','NINJA','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS']},'engine_metadata':json.loads((pkg/'engine.json').read_text()),'producer_sha256':hashlib.sha256(clang.read_bytes()).hexdigest(),'source_head':'82cdc7777e9dec4712b5913a8cfd31ea3634b99b','source_archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'tool_sha256':hashlib.sha256(tool.read_bytes()).hexdigest(),'sysroot_manifest_sha256':hashlib.sha256((root/'sysroot-progress.json').read_bytes()).hexdigest()}
(root/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
with(root/'build-kit.log').open('w') as log:
    p=subprocess.Popen(args,env=env,cwd='/home/speak/workspace/github/mcpp-language-server',stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    try:code=p.wait(timeout=1800)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid,signal.SIGKILL);p.wait();code=124
identity.update(status='complete' if code==0 else 'failed',exit_code=code,finished_unix=time.time())
if code==0:
    subprocess.run(['git','-C',str(source),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
    identity['kit_files']={str(path.relative_to(root/'kit')):hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted((root/'kit').rglob('*')) if path.is_file()}
    identity['kit_manifest_sha256']=hashlib.sha256((root/'kit/kit.json').read_bytes()).hexdigest()
(root/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
print('Same-source local Linux kit:',code,flush=True)
raise SystemExit(code)
