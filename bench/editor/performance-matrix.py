import subprocess,csv,pathlib
root=pathlib.Path.cwd()
cases=['63_editor_input.wire','64_editor_complete.wire','66_editor_members.wire','71_editor_goto.wire','72_editor_layers.wire','250_editor_journal.wire','356_editor_agent_context.wire']
cases=[c for c in cases if (root/'tests'/c).exists()]
bins={'base':'/tmp/vish-base-38dfc43/out/vish','head':'/tmp/vish-before-opt','after':str(root/'out/vish')}
with open(root/'bench/reports/editor-performance-matrix.csv','w') as out:
 w=csv.writer(out);w.writerow(['case','version','repeat','elapsed','user','system','rss_kb','exit'])
 for c in cases:
  for r in range(3):
   for label,binary in bins.items():
    p=subprocess.run(['/usr/bin/time','-f','METRIC %e %U %S %M','taskset','-c','0',binary,'test',str(root/'tests'/c)],capture_output=True,text=True)
    metric=[x for x in p.stderr.splitlines() if x.startswith('METRIC ')][-1].split()[1:]
    w.writerow([c,label,r+1,*metric,p.returncode]);out.flush()
    if p.returncode: print(c,label,p.stdout,p.stderr,flush=True)
 print('matrix complete',flush=True)
