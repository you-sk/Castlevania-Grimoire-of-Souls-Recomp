"""Convert private asset copies; original Data is never modified."""
from pathlib import Path
import argparse,collections,subprocess,sys,shutil
import UnityPy
repo=Path(__file__).resolve().parents[2];sys.path.insert(0,str(repo/'tools/shaders'))
import convert_shaders,convert_compute
ap=argparse.ArgumentParser();ap.add_argument('--data',type=Path,required=True);ap.add_argument('--hlslc',type=Path,required=True);ap.add_argument('--work',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
for p in [a.work.resolve(),a.out.resolve()]:
 if p==repo or repo in p.parents:raise SystemExit('Private conversion output must be outside the checkout')
if a.out.resolve()==a.data.resolve() or a.data.resolve() in a.out.resolve().parents:raise SystemExit('Output must not be inside original Data')
candidates=list(sorted((a.data/'StreamingAssets/ab').glob('*.ab')))+[a.data/n for n in ['globalgamemanagers','globalgamemanagers.assets','resources.assets','sharedassets0.assets','sharedassets1.assets','Resources/unity_builtin_extra']]
selected=[]
for p in candidates:
 if not p.is_file():continue
 env=UnityPy.load(str(p));kinds={x.type.name for x in env.objects}
 if 'Shader' in kinds or 'ComputeShader' in kinds:selected.append((p,'Shader' in kinds,'ComputeShader' in kinds))
if len({p.name for p,_,_ in selected})!=len(selected):raise SystemExit('Asset basename collision')
flat=a.work/'flat';flat.mkdir(parents=True,exist_ok=True);a.out.mkdir(parents=True,exist_ok=True)
convert_shaders.convert_files([str(p) for p,s,c in selected if s],str(a.hlslc),str(a.work/'shader'),str(flat))
for i,(p,s,c) in enumerate(selected):
 source=flat/p.name if s else p
 if c:
  env=UnityPy.load(str(source));convert_compute.convert_env(env,str(a.hlslc),str(a.work/'compute'/str(i)),collections.Counter())
  data=env.file.save(packer='lz4') if env.file.__class__.__name__=='BundleFile' else env.file.save()
 else:data=source.read_bytes()
 dest=a.out/p.relative_to(a.data);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
subprocess.run([sys.executable,'-I',str(repo/'tools/shaders/patch_build_settings.py'),str(a.data/'globalgamemanagers'),str(a.out/'globalgamemanagers')],check=True)
print('Converted asset copies:',len(selected),'No game data belongs in Git.')
