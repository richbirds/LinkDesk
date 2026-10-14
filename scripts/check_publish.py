#!/usr/bin/env python3
"""Fail closed if staged files include private data or unreviewed paths."""
import argparse, json, pathlib, re, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
ALLOWED_ROOT={'README.md','AGENTS.md','.gitignore','CMakeLists.txt'}
ALLOWED_DIR={'src','cmake','tests','server','third_party','assets','docs','examples','scripts','ForMac','ForWin'}
BLOCKED_PARTS={'config','private','deployment','proxy','dist','archive','toolcache','toolchain','passwords-local','__pycache__'}
def audit(files):
    private=[]
    config=ROOT/'config/linkdesk-server.json'
    if config.exists():
        try:
            values=json.loads(config.read_text())
            private=[str(values[k]).encode() for k in ('server','enrollmentKey','fingerprint') if len(str(values.get(k,'')))>=8]
        except (OSError,ValueError):
            raise SystemExit('Cannot read local private config for privacy comparison.')
    bad=[]
    for name in files:
        path=pathlib.PurePosixPath(name)
        forbidden=(path.is_absolute() or '..' in path.parts or (len(path.parts)==1 and name not in ALLOWED_ROOT)
            or (len(path.parts)>1 and path.parts[0] not in ALLOWED_DIR)
            or any(p in BLOCKED_PARTS or p.startswith('build') for p in path.parts[:-1])
            or path.suffix.lower() in {'.key','.pem','.p12','.pfx','.log','.zip','.gz','.bin'}
            or path.name in {'server.json','linkdesk-server.json','relay-config.json','.local-build.env'}
            or (ROOT/name).is_symlink())
        if forbidden: bad.append((name,'private or unreviewed path')); continue
        data=subprocess.check_output(['git','show',':'+name],cwd=ROOT)
        if any(value in data for value in private): bad.append((name,'matches local private configuration')); continue
        if path.suffix.lower() not in {'.png','.icns','.ico'}:
            text=data.decode('utf-8',errors='replace')
            patterns=[r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\s+[A-Za-z0-9+/]{64}',
                r'gh[pousr]_[A-Za-z0-9]{20,}',r'github_pat_[A-Za-z0-9_]{20,}',
                r'(?:/Users|/Volumes)/[A-Za-z0-9_.-]+/',r'"enrollmentKey"\s*:\s*"[a-fA-F0-9]{32,}"']
            if any(re.search(pattern,text) for pattern in patterns): bad.append((name,'possible private content'))
    if bad:
        for name,reason in bad: print(f'BLOCKED: {name}: {reason}',file=sys.stderr)
        raise SystemExit(1)
    print(f'Privacy review passed: {len(files)} staged files; no private matches.')
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--staged',action='store_true',required=True);parser.parse_args()
    names=subprocess.check_output(['git','diff','--cached','--name-only','--diff-filter=ACM','-z'],cwd=ROOT).decode().split('\0')
    audit([name for name in names if name])
