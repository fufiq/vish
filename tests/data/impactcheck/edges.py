#!/usr/bin/env python3
"""RA-3: ошибки CLI, импорт без графа, повторный rollback. Моё."""
import pathlib,subprocess,re,json,time,sys,tempfile
binary=str(pathlib.Path(sys.argv[1]).resolve())
out=pathlib.Path(sys.argv[2] if len(sys.argv)>2 else tempfile.mkdtemp(prefix='ra3-edges-'));out.mkdir(exist_ok=True)
base=out/'edges';base.mkdir(exist_ok=True)
a=base/'a.vish';b=base/'b.vish';d=base/'d.snapshot';a.write_text('a() => 1;\n');b.write_text('b() => 41;\n');d.write_text('a() => 3;\nnew() => 4;\n')
results=[]
def run(cmd,args):
 t=time.monotonic();r=subprocess.run([binary,cmd,'--impact']+list(map(str,args)),capture_output=True,text=True); n=len(results);(out/f'edge-{n}.log').write_text(r.stdout+r.stderr); results.append({'cmd':cmd,'args':list(map(str,args)),'exit':r.returncode,'seconds':time.monotonic()-t});return r
for cmd in ['modcheck','fmtcheck','nodecheck']:
 for args in [[],['--delta'],[a,'--delta','x',d],[a,'--delta','-1',d],[a,'--delta','9999999999999999999999',d],[a,'--delta','1',d],[a,'--wat'],[base/'absent'],[a,'--delta','0',base/'absent']]:
  r=run(cmd,args); assert r.returncode!=0,(cmd,args,r.stdout,r.stderr)
for content in ['use local;\na() => 1;\n','a() => (;\n','a() { use local; 1 }\n']:
 a.write_text(content)
 for cmd in ['modcheck','fmtcheck']:
  r=run(cmd,[a,b,'--delta',0,d,'--rollback','--rollback','--delta',0,d]); rows=re.findall(r'выполнено файлов (\d+), повторно использовано (\d+), всего (\d+), расхождения (\d+)',r.stdout)
  assert len(rows)==5 and all(x==('2','0','2','0') for x in rows),(cmd,content,rows,r.stderr)
  assert 'консервативный fallback' in r.stdout
(out/'edges.json').write_text(json.dumps(results,ensure_ascii=False,indent=2));print('edges PASS',len(results))
