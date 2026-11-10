#!/usr/bin/env python3
"""Execute prepared XNU's actual ADT reader/parser and compare its build policy."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tomllib
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    sys.path.insert(0, str(ROOT.parent / 'UniXNU/scripts'))
    from unixnu.driver_args import DRIVER_KEYS, READER_ABI, validate, from_config, report, require_kernel_reader
    from unixnu.util import Error
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = args.tree / '.cache/xnu-work/pexpert/gen/bootargs.c'
    header = source.with_name('uke_adt_driver_args.h').read_text()
    keys = re.findall(r'"([a-z0-9-]+)"', header.split('uke_adt_driver_keys[] = {', 1)[1].split('};', 1)[0])
    assert tuple(keys) == DRIVER_KEYS, 'kernel and payload allowlists diverged'
    (out / 'uke_adt_driver_args.h').write_text(header)
    cases = [f'{key}={value}' for key in DRIVER_KEYS for value in ('0', '1', '120', '0xffffffff', '0x2')]
    cases += ['', ' \t', 'sheng-buttons=0 sheng-touch=4', 'debug=334', 'serial=3', '-v',
              'sheng-buttons=1 sheng-buttons=0', 'sheng-buttons', 'sheng-buttons=',
              'sheng-buttons=-1', 'sheng-buttons=01', 'sheng-buttons=08', 'sheng-buttons=0b1',
              'sheng-buttons=0X1', 'sheng-buttons=0x', 'sheng-buttons=1k', 'sheng-buttons=1.5',
              'sheng-buttons=4294967296', 'sheng-buttons=0x100000000',
              'sheng-buttons=1\nsheng-touch=4', 'sheng-buttons=é', 'x' * 4096,
              'sheng-buttons=0' + ' ' * (4095 - len('sheng-buttons=0'))]
    rows = []
    for case in cases:
        try:
            validate(case)
            valid = True
        except Error:
            valid = False
        rows.append('{'+json.dumps(case)+','+str(int(valid))+'}')
    for kind in ('bytes', 'u32', 'strings'):
        try:
            from_config({'adt': {'chosen': {kind: {'unixnu-driver-args': 'bad'}}}})
        except Error:
            pass
        else:
            raise AssertionError('wrong property type accepted')
    assert report('sheng-buttons=0', 'sheng-buttons=1')['driver_args_effective']['sheng-buttons'] == dict(value='0', source='command_line')
    require_kernel_reader(b'old kernel', '')  # Old payload/command-line path stays compatible.
    require_kernel_reader(b'new kernel' + READER_ABI, 'sheng-buttons=1')
    try:
        require_kernel_reader(b'old kernel', 'sheng-buttons=1')
    except Error:
        pass
    else:
        raise AssertionError('old kernel with ADT-only defaults accepted')
    from unixnu.devicetree import generate
    try:
        generate(Path('/nonexistent/adt-policy-negative.dtb'),
                 {'adt': {'chosen': {'string': {'unixnu-driver-args': 'debug=0'}}}})
    except Error as error:
        assert 'unsupported ADT driver argument' in str(error)
    else:
        raise AssertionError('ADT generator accepted a reserved kernel key')
    from unixnu import payload
    old_kernel = ROOT.parent / 'uke-port/releases/2026-11-10-input-backlight-runtime-append/kernel-raw.macho'
    context = SimpleNamespace(board={'payload': {'kernel_args': []},
        'adt': {'chosen': {'string': {'unixnu-driver-args': 'sheng-buttons=1'}}}},
        args=SimpleNamespace(kernel_arg=[]))
    try:
        payload.build(context, kernel=old_kernel)
    except Error as error:
        assert 'require a kernel with unixnu-driver-args-v1' in str(error)
    else:
        raise AssertionError('payload builder accepted an actual legacy kernel')
    body = source.read_text().split('/* function: get_range_bounds', 1)[0]
    body = re.sub(r'^#include <.*\n', '', body, flags=re.M)
    body = body.replace('__attribute__((weak)) char PE_builtin_boot_args[] = "";', 'char PE_builtin_boot_args[512] = "";')
    prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#undef __x86_64__
typedef int boolean_t;
#define TRUE 1
#define FALSE 0
typedef void *DTEntry;
#define kSuccess 0
static char cli[2048];
static char *PE_boot_args(void) {return cli;}
static const char *property;
static unsigned property_size;
static int ready, lookups;
static int SecureDTLookupEntry(void *a,const char *path,DTEntry *entry) {
 (void)a;++lookups;if(!ready || strcmp(path,"/chosen"))return -1;*entry=(void*)1;return 0;
}
static int SecureDTGetProperty(DTEntry a,const char *key,const void **data,unsigned *size) {
 (void)a;if(strcmp(key,"unixnu-driver-args")||!property)return -1;*data=property;*size=property_size;return 0;
}
int IODTGetDefault(const char*a,void*b,unsigned c) {(void)a;(void)b;(void)c;return -1;}
'''
    maincode = r'''
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line%u: %s\n",__LINE__,#x);exit(1);}}while(0)
static void prop(const char *s) {property=s;property_size=(unsigned)strlen(s)+1;}
int main(void) {
 struct {const char *text;int valid;} cases[]={CASES};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
  CHECK(uke_adt_driver_valid(cases[i].text,(unsigned)strlen(cases[i].text)+1)==cases[i].valid);
 CHECK(!uke_adt_driver_valid(NULL,1));
 ready=1;
 FULL_CONFIG_TEST
 for(unsigned i=0;i<sizeof(uke_adt_driver_keys)/sizeof(uke_adt_driver_keys[0]);++i) {
  const char *key=uke_adt_driver_keys[i];char text[128];unsigned value=99;
  snprintf(text,sizeof(text),"%s=120",key);prop(text);
  CHECK(PE_parse_boot_argn(key,&value,4)&&value==120);
  char string[16];CHECK(PE_parse_boot_arg_str(key,string,sizeof(string))&&!strcmp(string,"120"));
  snprintf(cli,sizeof(cli),"%s=0",key);CHECK(PE_parse_boot_argn(key,&value,4)&&value==0);cli[0]=0;
  CHECK(PE_parse_boot_argn(key,NULL,0));
 }
 strcpy(PE_builtin_boot_args,"sheng-buttons=2 owner-staged-apps=1 debug=334");
 unsigned value=99;prop("sheng-buttons=0");
 CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==0);
 ready=0;CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==2);
 ready=1;CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==0);
 prop("sheng-buttons=1");CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==1); /* new DT address/view */
 int before=lookups;CHECK(PE_parse_boot_argn("debug",&value,4)&&value==334&&lookups==before);
 CHECK(PE_parse_boot_argn("owner-staged-apps",&value,4)&&value==1&&lookups==before);
 prop("debug=0 sheng-buttons=0");CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==2);
 prop("sheng-buttons=0");--property_size;CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==2);
 property="sheng-buttons=0\0junk";property_size=21;CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==2);
 property_size=4097;CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==2);
 prop("sheng-buttons=1");unsigned char bytes[]={0xaa,0,0xbb};
 CHECK(PE_parse_boot_argn("sheng-buttons",bytes+1,1)&&bytes[1]==1&&bytes[0]==0xaa&&bytes[2]==0xbb);
 uint64_t wide=0;CHECK(PE_parse_boot_argn("sheng-buttons",&wide,8)&&wide==1);
 property=NULL;CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==2);
 value=99;CHECK(!PE_parse_boot_argn("not-present",&value,4)&&value==99);
 strcpy(cli,"sheng-buttons=0 sheng-buttons=1");CHECK(PE_parse_boot_argn("sheng-buttons",&value,4)&&value==1);
 puts("PASS production XNU ADT parser, numeric/string/width/precedence/early-DT and build-policy parity");
 printf("checks=%u\n",checks);
}
'''.replace('CASES', ',\n'.join(rows))
    full = from_config(tomllib.loads((ROOT / 'board.toml').read_text()))
    full_checks = 'prop(' + json.dumps(full) + ');\n'
    for token in full.split():
        key, value = token.split('=')
        expected = int(value, 16 if value.startswith('0x') else 10)
        full_checks += '{unsigned v=99;CHECK(PE_parse_boot_argn(' + json.dumps(key) + ',&v,4)&&v==' + str(expected) + 'u);}\n'
    full_checks += 'ready=0;property=NULL;strcpy(cli,' + json.dumps(full) + ');\n'
    for token in full.split():
        key, value = token.split('=')
        expected = int(value, 16 if value.startswith('0x') else 10)
        full_checks += '{unsigned v=99;CHECK(PE_parse_boot_argn(' + json.dumps(key) + ',&v,4)&&v==' + str(expected) + 'u);}\n'
    full_checks += 'cli[0]=0;ready=1;\n'
    maincode = maincode.replace('FULL_CONFIG_TEST', full_checks)
    test = out / 'adt-driver-args.c'
    test.write_text(prefix + body + maincode)
    subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-Wno-unused-function', str(test), '-o', str(out / 'adt-driver-args')], check=True)
    result = subprocess.run([str(out / 'adt-driver-args')], text=True, capture_output=True)
    (out / 'host-test.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    print(result.stdout.strip())
    (out / 'host-test.json').write_text(json.dumps(dict(passed=True, policy_keys=len(keys),
        validation_cases=len(cases), sanitizers=['ASan', 'UBSan'], hardware_verified=False), indent=2)+'\n')


if __name__ == '__main__':
    main()
