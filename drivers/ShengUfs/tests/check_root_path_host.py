#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Bounded ASan/UBSan checks of production root layout and window reads."""
import argparse
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',required=True,type=Path)
parser.add_argument('--openssl-prefix',type=Path)
parser.add_argument('--root-image',type=Path,help='Optional existing image: read only its first 4096 bytes')
parser.add_argument('--verified-root',type=Path,help='UniXNU kits/VerifiedRoot checkout for the table cache check')
args=parser.parse_args()
source=Path(__file__).resolve().parent
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
openssl=args.openssl_prefix or Path(subprocess.check_output(['brew','--prefix','openssl@3'],text=True).strip())
for name in ('RootLayout','RootWindowRead','RootDiagnostics','RootPublication'):
    binary=out/name
    command=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(source/(name+'.cpp')),'-o',str(binary)]
    if name=='RootLayout':command+=['-I',str(openssl/'include'),'-L',str(openssl/'lib'),'-lcrypto']
    subprocess.run(command,check=True,timeout=60)
    command=[str(binary)]
    if name=='RootLayout' and args.root_image:command.append(str(args.root_image.resolve()))
    with (out/(name+'.log')).open('w') as log:subprocess.run(command,check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT)
    print('PASS:',name)
with (out/'media-diagnostics.txt').open('w') as log:
    subprocess.run([str(out/'RootDiagnostics'),'--media'],check=True,timeout=20,stdout=log,stderr=subprocess.STDOUT)
print('PASS: maximum-width diagnostics plus eight media entries prepared for actual renderer')
if args.verified_root:
    kit=args.verified_root.resolve();binary=out/'VerifiedTableCache'
    subprocess.run(['clang','-c','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(kit/'include'),
                    str(kit/'src'/'verified_root.c'),'-o',str(out/'verified_root.o')],check=True,timeout=60)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I',str(kit/'include'),
                    '-I',str(openssl/'include'),str(source/'VerifiedTableCache.cpp'),str(out/'verified_root.o'),
                    '-L',str(openssl/'lib'),'-lcrypto','-o',str(binary)],check=True,timeout=60)
    with (out/'VerifiedTableCache.log').open('w') as log:subprocess.run([str(binary)],check=True,timeout=60,stdout=log,stderr=subprocess.STDOUT)
    print('PASS: VerifiedTableCache')
else:
    print('SKIP: VerifiedTableCache (needs --verified-root)')
