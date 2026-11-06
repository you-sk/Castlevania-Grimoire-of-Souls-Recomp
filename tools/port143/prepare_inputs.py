"""Extract and verify private inputs from an owner's expanded macOS app."""
from pathlib import Path
import argparse,hashlib,json,plistlib,struct
profile=json.loads(Path(__file__).with_name('profile.json').read_text())
ap=argparse.ArgumentParser();ap.add_argument('--contents',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
root=Path(__file__).resolve().parents[2];out=a.out.resolve();contents=a.contents.resolve()
if out==root or root in out.parents:raise SystemExit('Private inputs must be outside the repository')
info=plistlib.loads((contents/'Info.plist').read_bytes())
if str(info.get('CFBundleShortVersionString'))!='1.4.3' or str(info.get('CFBundleVersion'))!='105':raise SystemExit('Expected version 1.4.3 build 105')
def thin(path):
 data=path.read_bytes()
 if data[:4] in (b'\xca\xfe\xba\xbe',b'\xca\xfe\xba\xbf'):
  wide=data[3]==0xbf;stride=32 if wide else 20
  for i in range(struct.unpack_from('>I',data,4)[0]):
   at=8+i*stride;cpu=struct.unpack_from('>I',data,at)[0]
   offset,size=struct.unpack_from('>QQ' if wide else '>II',data,at+8)
   if cpu==0x100000c:return data[offset:offset+size]
  raise SystemExit('Missing arm64 architecture')
 return data
inputs={'GameAssembly_arm64.dylib':(thin(contents/'Frameworks/GameAssembly.dylib'),profile['game_arm64_sha256']),
 'SPFXUnityPlugin_arm64.macho':(thin(contents/'PlugIns/SPFXUnityPlugin.bundle/Contents/MacOS/SPFXUnityPlugin'),profile['spfx_arm64_sha256']),
 'global-metadata.dat':((contents/'Resources/Data/il2cpp_data/Metadata/global-metadata.dat').read_bytes(),profile['metadata_sha256'])}
for name,(data,expected) in inputs.items():
 if hashlib.sha256(data).hexdigest()!=expected:raise SystemExit('Input mismatch: '+name)
out.mkdir(parents=True,exist_ok=True)
for name,(data,expected) in inputs.items():
 dest=out/name
 if dest.exists() and dest.read_bytes()!=data:raise SystemExit('Refusing overwrite: '+name)
 dest.write_bytes(data)
print('Verified 1.4.3 build 105 inputs written locally. No receipt is read or copied.')
