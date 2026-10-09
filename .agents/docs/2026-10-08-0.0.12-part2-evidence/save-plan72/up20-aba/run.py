from pathlib import Path
import subprocess,sys,json,time
root=Path(__file__).parent
for label in ['baseline-before','candidate','baseline-after']:
 print('Starting '+label,flush=True)
 code=subprocess.call([sys.executable,str(root/label/'run.py')])
 if code:raise SystemExit(code)
 print(label+' complete',flush=True)
(root/'completed.json').write_text(json.dumps({'all_arms_exit_zero':True,'finished_unix':time.time()},indent=2)+'\n')
