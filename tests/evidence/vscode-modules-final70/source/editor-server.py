#!/home/speak/.xlings/data/xpkgs/xim-x-python/3.13.12/bin/python3
import os,sys
server='/home/speak/workspace/github/mcpp-language-server/target/x86_64-linux-gnu/c6a96775022a4395/bin/mcppls'
os.execv(server,[server,*sys.argv[1:],"--clangd",'/tmp/mcppls-part2-floor70/dist/clangd-23.1.0-mcppls.0-linux-x64/clangd/bin/clangd',"--kit",'/home/speak/.local/share/mcppls/payload/kit',"--no-discover"])
