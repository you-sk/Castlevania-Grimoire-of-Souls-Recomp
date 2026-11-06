"""Assemble a private Windows installation from local owner-supplied files."""
from pathlib import Path
import argparse,hashlib,json,shutil
repo=Path(__file__).resolve().parents[2];profile=json.loads(Path(__file__).with_name('profile.json').read_text())
ap=argparse.ArgumentParser();ap.add_argument('--data',type=Path,required=True);ap.add_argument('--converted',type=Path,required=True);ap.add_argument('--inputs',type=Path,required=True);ap.add_argument('--unity-win64',type=Path,required=True);ap.add_argument('--game-dll',type=Path,required=True);ap.add_argument('--spfx-dll',type=Path,required=True);ap.add_argument('--apple-plugins',type=Path,required=True);ap.add_argument('--tools',type=Path,required=True);ap.add_argument('--save-source',type=Path);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
out=a.out.resolve()
if out==repo or repo in out.parents:raise SystemExit('Private installation must be outside checkout')
if out.exists() and any(out.iterdir()):raise SystemExit('Output must be empty; existing saves are never overwritten')
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for chunk in iter(lambda:f.read(8*1024*1024),b''):h.update(chunk)
 return h.hexdigest()
for name,key in [('GameAssembly_arm64.dylib','game_arm64_sha256'),('SPFXUnityPlugin_arm64.macho','spfx_arm64_sha256'),('global-metadata.dat','metadata_sha256')]:
 if sha(a.inputs/name)!=profile[key]:raise SystemExit('Input mismatch: '+name)
if sha(a.data/'il2cpp_data/Metadata/global-metadata.dat')!=profile['metadata_sha256']:raise SystemExit('Wrong game Data')
game=out/'game';game.mkdir(parents=True)
def copy(src,dest):
 if src.is_symlink():raise SystemExit('Symlink input refused: '+str(src))
 dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
for base in [a.data,a.converted]:
 for path in base.rglob('*'):
  if path.is_file():
   if any(x.lower() in {'_masreceipt','receipt','saves','savedata'} for x in path.relative_to(base).parts):raise SystemExit('Private non-game input found')
   copy(path,game/'GoS_Data'/path.relative_to(base))
for src,dest in [(a.unity_win64/'WindowsPlayer.exe',game/'GoS.exe'),(a.unity_win64/'UnityPlayer.dll',game/'UnityPlayer.dll'),(a.unity_win64/'baselib.dll',game/'baselib.dll'),(a.unity_win64/'Data/Resources/unity default resources',game/'GoS_Data/Resources/unity default resources'),(a.game_dll,game/'GameAssembly.dll'),(a.inputs/'GameAssembly_arm64.dylib',game/'GameAssembly_arm64.dylib'),(a.spfx_dll,game/'GoS_Data/Plugins/x86_64/SPFXUnityPlugin.dll'),(a.inputs/'SPFXUnityPlugin_arm64.macho',game/'GoS_Data/Plugins/x86_64/SPFXUnityPlugin.macho')]:copy(src,dest)
for path in a.apple_plugins.glob('*.dll'):copy(path,game/'GoS_Data/Plugins/x86_64'/path.name)
for name in ['isolated_launch.exe','network_probe.exe']:copy(a.tools/name,out/'tools'/name)
for name in ['Setup-Firewall.ps1','Play-Isolated.ps1']:copy(repo/'tools/windows143'/name,out/name)
(out/'Play.cmd').write_text('@echo off\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Play-Isolated.ps1"\nif errorlevel 1 pause\n')
save=out/'saves/quest2-3';save.mkdir(parents=True)
if a.save_source:
 for name in ['chatDateList.json','gdUserSaveData00.json','gdUserSaveData00_backup.json','option.json','title.json']:
  if (a.save_source/name).is_file():copy(a.save_source/name,save/name)
manifest={str(p.relative_to(game)).replace('\\','/'):sha(p) for p in game.rglob('*') if p.is_file()}
for name in ['isolated_launch.exe','network_probe.exe']:manifest['../tools/'+name]=sha(out/'tools'/name)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
print('Private installation staged:',len(manifest),'hashed files. Configure per-program firewall before Play.cmd.')
