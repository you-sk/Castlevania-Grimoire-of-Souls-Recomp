"""Regenerate private game code from the owner's verified 1.4.3 arm64 image.

No dumps, function tables or generated C++ are distributed by this repository.
Use a work directory outside the checkout. This command never launches a game.
"""
from pathlib import Path
import argparse,bisect,hashlib,json,subprocess,sys,re
import lief

REPO=Path(__file__).resolve().parents[2]
PROFILE=json.loads(Path(__file__).with_name('profile.json').read_text())
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--binary',type=Path,required=True,help='Locally extracted, thin arm64 GameAssembly.dylib')
ap.add_argument('--dump',type=Path,required=True,help='Local Il2CppDumper directory containing script.json and il2cpp.h')
ap.add_argument('--work',type=Path,required=True)
ap.add_argument('--analyze-only',action='store_true')
args=ap.parse_args();work=args.work.resolve();binary=args.binary.resolve();dump=args.dump.resolve()
if work==REPO or REPO in work.parents:raise SystemExit('Keep private work outside the checkout')
if hashlib.sha256(binary.read_bytes()).hexdigest()!=PROFILE['game_arm64_sha256']:raise SystemExit('Unsupported input: exact verified 1.4.3 build 105 arm64 image required')
for name in ['script.json','il2cpp.h']:
 if not (dump/name).is_file():raise SystemExit('Missing local Il2CppDumper output: '+name)
work.mkdir(parents=True,exist_ok=True)
def run(tool,*values):subprocess.run([sys.executable,'-I',str(REPO/tool),*map(str,values)],check=True)
registration=work/'registration.json';table=work/'method-table.json';classification=work/'classification.json'
run('tools/recomp/registration_tables.py',binary,PROFILE['code_registration'],registration,PROFILE['metadata_registration'])
run('tools/analysis/function_table.py',binary,dump/'script.json',table)
run('tools/recomp/classify_text.py',binary,registration,table,classification)
image=lief.parse(str(binary));starts=sorted(set(image.function_starts.functions));text=image.get_section('__text');end=text.virtual_address+text.size
original=json.loads(table.read_text());classes=json.loads(classification.read_text());functions={x['addr']:x for x in original['functions']}
methods={}
for m in json.loads((dump/'script.json').read_text(encoding='utf-8-sig'))['ScriptMethod']:methods.setdefault(m['Address'],m['Name'])
for address in classes['generated']:
 stop=min(starts[bisect.bisect_right(starts,address)],end)
 functions[address]={'addr':address,'size':stop-address,'name':methods.get(address,f'generated_text_{address:x}')}
main=work/'main-functions.json';main.write_text(json.dumps({'functions':sorted(functions.values(),key=lambda x:x['addr']),'imports':original['imports']}))
if len(functions)!=PROFILE['expected_main_functions']:raise SystemExit('Function count differs from validated input; stop and investigate')
gap=[]
for address in PROFILE['gap_functions']:
 if address not in starts:raise SystemExit('Helper is not a function start')
 gap.append({'addr':address,'size':starts[bisect.bisect_right(starts,address)]-address,'name':f'helper_{address:x}'})
(work/'gap-functions.json').write_text(json.dumps({'functions':gap}))
run('tools/recomp/lsda_143.py',binary,work/'lsda.json')
run('tools/recomp/classify_types.py',dump/'il2cpp.h',dump/'script.json',work/'types.json')
print('Validated main functions:',len(functions),'helpers:',len(gap),flush=True)
if args.analyze_only:raise SystemExit(0)
run('tools/recomp/gen_bridges.py',registration,dump/'script.json',work/'types.json',work/'bridges')
translate=['tools/recomp/arm64_recomp_143.py',binary,main,work/'generated','--split','512','--signatures',dump/'script.json','--types',work/'types.json','--native-resolvers','0x197e98,0x1983d4','--lsda',work/'lsda.json','--wrapper-typeinfo',PROFILE['wrapper_typeinfo'],'--override','0x1820650=LocalServerMoveNext']
for hook in ['0x201e934=HookIsTrigger','0x201e360=HookJoystickInput','0x1e84b48=HookRebindInputSystemKeyDown','0x1c0ea94=HookKeyboardInputText143','0x2f6b350=HookSimpleCameraVRev143','0x2f69064=HookCommandBufferOnEnable143']:translate+=['--hook',hook]
run(*translate)
run('tools/recomp/arm64_recomp_143.py',binary,work/'gap-functions.json',work/'gap','--split','4','--extra-table','--lsda',work/'lsda.json','--wrapper-typeinfo',PROFILE['wrapper_typeinfo'])
# Direct calls between helper functions can bypass RegisterImport. Redirect
# every native-bound helper body to the same verified Windows endpoint.
source=(REPO/'recomp/winbuild/runtime_imports.cpp').read_text()
native={int(v,16) for v in re.findall(r'RegisterImport\((0x[0-9a-f]+)',source)}
for path in (work/'gap').glob('recomp_*.cpp'):
 value=path.read_text()
 for address in native:
  value=re.sub(rf'void f_{address:x}\(Ctx& c\)\n\{{.*?\n\}}',lambda m:f'void f_{address:x}(Ctx& c)\n{{\n    CallImport(c, {address:#x});\n}}',value,flags=re.S)
 path.write_text(value)
print('Generated private C++ under',work,'; never commit this directory.')
