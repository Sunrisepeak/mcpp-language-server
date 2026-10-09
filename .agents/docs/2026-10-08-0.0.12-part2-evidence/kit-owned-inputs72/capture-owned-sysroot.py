from pathlib import Path
import subprocess,shutil,json
root=Path('/out');target=root/'sysroot-owned';assert not target.exists();target.mkdir()
names=subprocess.check_output(['dpkg','-L','libc6-dev','linux-libc-dev'],text=True).splitlines()
owned=sorted({name for name in names if name.startswith('/usr/include/') and Path(name).is_file()})
assert owned
for name in owned:
    src=Path(name);dst=target/name.lstrip('/');dst.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(src,dst,follow_symlinks=True)
(root/'sysroot-owned-paths.json').write_text(json.dumps(owned,indent=2)+'\n')
(root/'sysroot-owned-packages.txt').write_text(subprocess.check_output(['dpkg-query','-W','libc6-dev','linux-libc-dev'],text=True))
print('Owned C/kernel include files:',len(owned))
