from pathlib import Path
import os
import sys,json
R=Path(__file__).resolve().parents[3];sys.path.insert(0,str(R/'tools/recomp'))
import arm64_recomp_143 as t
em=t.Emitter(None,set(),0);em.icall_cache={};em.types={}
em.sig_by_name={
 'Example.Type$$Instance':['int32_t Method (Example_o* __this, int32_t value, const MethodInfo* method);'],
 'Example.Type$$Static':['int32_t Method (int32_t value, const MethodInfo* method);'],
 'Example.Type$$Ambiguous':['int32_t A (int32_t value, const MethodInfo* method);','float B (float value, const MethodInfo* method);']}
checks=[('Example.Type::Instance(System.Int32)',('int32_t',['P','int32_t'])),('Example.Type::Static(System.Int32)',('int32_t',['int32_t'])),('Example.Type::Instance',('int32_t',['P','int32_t'])),('Example.Type::Ambiguous',None)]
for name,expected in checks:assert t.icall_signature(em,name)==expected,(name,t.icall_signature(em,name),expected)
(Path(os.environ["GOSR_BUILD_ROOT"])/'icall-signature-tests-143.json').write_text(json.dumps({'passed':len(checks),'game_executed':False},indent=2));print(len(checks),'passed')
