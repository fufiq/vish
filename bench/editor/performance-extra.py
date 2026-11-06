import subprocess,csv,pathlib
root=pathlib.Path.cwd();bins={'base':'/tmp/vish-base-38dfc43/out/vish','head':'/tmp/vish-before-opt','after':str(root/'out/vish')}
with open(root/'bench/reports/editor-performance-extra.csv','w') as f:
 w=csv.writer(f);w.writerow(['case','version','repeat','elapsed','user','system','rss_kb','exit'])
 for case in ['open','complete']:
  for r in range(3):
   for v,b in bins.items():
    p=subprocess.run(['/usr/bin/time','-f','METRIC %e %U %S %M','taskset','-c','0',b,'test',str(root/f'bench/reports/{case}.wire')],capture_output=True,text=True)
    m=[l for l in p.stderr.splitlines() if l.startswith('METRIC')][-1].split()[1:];w.writerow([case,v,r+1,*m,p.returncode]);f.flush()
 for v,b in bins.items():
  p=subprocess.run([b,'test',str(root/'bench/reports/journal.wire')],capture_output=True,text=True);(root/f'bench/reports/editor-journal-{v}.log').write_text(p.stdout+p.stderr)
 for r in range(2):
  for v,b in bins.items():
   p=subprocess.run(['bench/editor/keys.sh',b],capture_output=True,text=True);(root/f'bench/reports/editor-keys-{v}-{r+1}.log').write_text(p.stdout+p.stderr)
