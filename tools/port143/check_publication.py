"""Audit the Git index before commit/push. Never scans or uploads private work."""
from pathlib import Path
import re,subprocess,sys
repo=Path(__file__).resolve().parents[2]
paths=subprocess.check_output(['git','ls-files','-z'],cwd=repo).decode().split('\0')
bad=[];count=0
suffixes={'.exe','.dll','.dylib','.macho','.zip','.7z','.dmg','.ipa','.dat','.assets','.ab','.sa','.obj','.pdb','.map','.gpatch','.png','.jpg','.mp4','.wav','.mp3'}
for relative in filter(None,paths):
 p=Path(relative);data=subprocess.check_output(['git','show',':'+relative],cwd=repo);count+=1
 if p.suffix.lower() in suffixes or any(x.lower() in {'work','game','dump','saves','savedata','_masreceipt','generated','private','__pycache__'} for x in p.parts):bad.append((relative,'private/binary path'))
 if b'\0' in data:bad.append((relative,'binary content'))
 try:text=data.decode('utf-8')
 except UnicodeDecodeError:bad.append((relative,'non-UTF8 content'));continue
 if re.search(r'(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{30,})',text):bad.append((relative,'credential pattern'))
 if re.search(r'/home/[A-Za-z0-9_-]+/|/mnt/[a-z]/Users/[^/]+/|[A-Za-z]:\\Users\\[^\\]+\\',text):bad.append((relative,'personal absolute path'))
 if re.search(r'void f_[0-9a-f]+\(Ctx& c\)\s*\{\s*// [0-9a-f]+:',text):bad.append((relative,'translated game body'))
if bad:
 for p,reason in bad:print(p,reason)
 raise SystemExit(1)
print('Index audit passed:',count,'text source/documentation files; no game binaries, saves, receipts or personal build paths.')
