"""Differential tests of newly emitted CRC instructions, not game execution."""
from pathlib import Path
import os
import subprocess,re,sys,ctypes,random,json
R=Path(__file__).resolve().parents[3];B=Path(os.environ["GOSR_BUILD_ROOT"]);D=B/'work/crc-tests';D.mkdir(exist_ok=True)
sys.path.insert(0,str(R/'tools/recomp'))
import arm64_recomp_143 as translator
import capstone
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_NZCV
md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM);md.detail=True
em=translator.Emitter(md,set(),0x10000)
functions=[];variants=[]
for m in ['crc32b','crc32h','crc32w','crc32x','crc32cb','crc32ch','crc32cw','crc32cx']:
    for dest in [0,1,2]:
        assembly=f'{m} w{dest}, w1, '+('x2' if m.endswith('x') else 'w2')
        s=subprocess.run(['/usr/bin/llvm-mc-18','--triple=aarch64','--mattr=+crc','--show-encoding'],input=assembly,text=True,capture_output=True,check=True).stdout
        encoded=bytes(int(x,16) for x in re.search(r'encoding: \[(.*?)\]',s).group(1).split(','))
        ins=next(md.disasm(encoded,0x1000))
        body=' '.join(em.translate(ins,{'start':0x1000,'end':0x1004}))
        idx=len(variants)
        functions.append(f'extern "C" uint64_t crc_{idx}(uint64_t seed,uint64_t value) {{ Ctx c{{}}; c.x[1]=seed; c.x[2]=value; {body} return c.x[{dest}]; }}')
        variants.append((assembly,encoded,dest))
source='#include <cstdint>\nstruct Ctx { uint64_t x[31]; };\n'+'\n'.join(functions)
(D/'crc.cpp').write_text(source)
subprocess.run(['/usr/bin/clang++-18','-std=c++17','-O2','-shared','-fPIC',str(D/'crc.cpp'),'-o',str(D/'crc.so')],check=True)
lib=ctypes.CDLL(str(D/'crc.so'));rng=random.Random(143)
vectors=[(a,b) for a in [0,1,0xffffffff,0xffffffffffffffff] for b in [0,1,0xff,0xffffffff,0xffffffffffffffff]]+[(rng.getrandbits(64),rng.getrandbits(64)) for _ in range(128)]
tests=0
for idx,(assembly,encoded,dest) in enumerate(variants):
    fn=getattr(lib,f'crc_{idx}');fn.argtypes=[ctypes.c_uint64,ctypes.c_uint64];fn.restype=ctypes.c_uint64
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);uc.mem_map(0x1000,4096);uc.mem_write(0x1000,encoded)
    for seed,value in vectors:
        uc.reg_write(UC_ARM64_REG_X0,0xffffffffffffffff);uc.reg_write(UC_ARM64_REG_X1,seed);uc.reg_write(UC_ARM64_REG_X2,value);uc.reg_write(UC_ARM64_REG_NZCV,0xa0000000)
        uc.emu_start(0x1000,0x1004,count=1)
        expected=uc.reg_read([UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2][dest]);actual=fn(seed,value)
        assert actual==expected,(assembly,seed,value,actual,expected)
        assert uc.reg_read(UC_ARM64_REG_NZCV)==0xa0000000
        tests+=1
result={'variants':len(variants),'vectors_per_variant':len(vectors),'passed':tests,'reference':'Unicorn 2.1.4 ARM64; synthetic CRC instructions only','compiler':'clang++-18 -O2 x86_64 Linux','game_executed':False}
(D/'crc-differential-tests-143.json').write_text(json.dumps(result,indent=2));print(result)
