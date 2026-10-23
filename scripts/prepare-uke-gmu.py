#!/usr/bin/env python3
"""Prepare the Adreno 732 (uke / SM7675) inputs for NativeAdreno.

Two modes, both CPU-only and without device I/O:

  --derive  read the live DTB and the pinned Linux Adreno sources, then write
            scripts/data/uke-gpu-inputs.json (the recorded board facts).
  default   read that JSON and board.toml, validate the firmware pins if the
            files are present, and write the generated headers consumed by
            kits/uke/ShengGmu.c:

              build/sheng-gmu/ShengGmuSource.h
              build/sheng-gmu/ShengGmuVotesSource.h
              build/sheng-gmu/ShengGmuCommandDbSource.h

The derive mode is what the maintainer runs against uke-live.dtb and the
pinned source; the build mode is what CI runs. No firmware byte enters Git.
"""
import argparse
import ast
import hashlib
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'scripts/data/uke-gpu-inputs.json'
OUT = ROOT / 'build/sheng-gmu'
FIRMWARE = ROOT / '.cache/firmware/uke-gpu'
NS = {'r': 'http://nouveau.freedesktop.org/'}
ORDER = ['A2XX', 'A3XX', 'A4XX', 'A5XX', 'A6XX', 'A7XX', 'A8XX']
GPU_NODE = '/soc/qcom,kgsl-3d0@3d00000'
GMU_NODE = '/soc/qcom,gmu@3d69000'
SMMU_NODE = '/soc/qcom,kgsl-iommu@3da0000'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fdtget(dtb, node, prop, kind='x'):
    return subprocess.check_output(['fdtget', '-t', kind, str(dtb), node, prop],
                                   text=True, stderr=subprocess.DEVNULL).strip()


def cells(dtb, node, prop):
    return [int(v, 16) for v in fdtget(dtb, node, prop).split()]


def one(dtb, node, prop):
    values = cells(dtb, node, prop)
    if len(values) != 1:
        raise SystemExit('%s:%s has %d cells, expected 1' % (node, prop, len(values)))
    return values[0]


def text(dtb, node, prop):
    return fdtget(dtb, node, prop, 's').split()


def count_prop(dtb, node, prop):
    try:
        return one(dtb, node, prop)
    except (subprocess.CalledProcessError, SystemExit):
        parent = node.rsplit('/', 1)[0] or '/'
        return one(dtb, parent, prop)


def ranges(dtb, node):
    values = cells(dtb, node, 'reg')
    address_cells = count_prop(dtb, node, '#address-cells')
    size_cells = count_prop(dtb, node, '#size-cells')
    per_window = address_cells + size_cells
    if per_window < 2 or len(values) % per_window:
        raise SystemExit('%s: reg does not match %d/%d cells' %
                         (node, address_cells, size_cells))
    output = []
    for start in range(0, len(values), per_window):
        base = 0
        for cell in range(address_cells):
            base = (base << 32) | values[start + cell]
        size = 0
        for cell in range(size_cells):
            size = (size << 32) | values[start + address_cells + cell]
        output.append(dict(base=base, bytes=size))
    return output


def named_ranges(dtb, node):
    names = text(dtb, node, 'reg-names')
    regs = ranges(dtb, node)
    if len(names) != len(regs):
        raise SystemExit('%s: reg-names/reg count differs' % node)
    return {name: reg for name, reg in zip(names, regs)}


def children(dtb, node):
    return subprocess.check_output(['fdtget', '-l', str(dtb), node], text=True).split()


def reserved_with_compatible(dtb, compatible):
    for child in children(dtb, '/reserved-memory'):
        node = '/reserved-memory/' + child
        try:
            values = text(dtb, node, 'compatible')
        except subprocess.CalledProcessError:
            continue
        if compatible in values:
            return node
    raise SystemExit('no reserved-memory node with compatible ' + compatible)


def reserved_with_phandle(dtb, phandle):
    for child in children(dtb, '/reserved-memory'):
        node = '/reserved-memory/' + child
        try:
            value = one(dtb, node, 'phandle')
        except (subprocess.CalledProcessError, SystemExit):
            continue
        if value == phandle:
            return node
    raise SystemExit('no reserved-memory node with phandle 0x%x' % phandle)


def soc_node_with_compatible(dtb, compatible):
    for child in children(dtb, '/soc'):
        node = '/soc/' + child
        try:
            values = text(dtb, node, 'compatible')
        except subprocess.CalledProcessError:
            continue
        if compatible in values:
            return node
    raise SystemExit('no /soc node with compatible ' + compatible)


# ---------------------------------------------------------------- register map

def matched(variants):
    if variants is None:
        return True
    for part in variants.split(','):
        part = part.strip()
        if '-' in part:
            left, right = part.split('-', 1)
            low = ORDER.index(left) if left else 0
            high = ORDER.index(right) if right else len(ORDER) - 1
            if low <= ORDER.index('A7XX') <= high:
                return True
        elif part == 'A7XX':
            return True
    return False


def resolve(xml_dir, name, files, kind='reg(?:32|64)', variant=None):
    hits = set()
    for filename in list(dict.fromkeys([*files, 'a6xx_gmu.xml'])):
        source = (xml_dir / filename).read_text()
        for match in re.finditer(r'<(' + kind + r') offset="(0x[0-9a-fA-F]+)" name="' +
                                 name + r'"([^>]*)>', source):
            found = re.search(r'variants="([^"]+)"', match.group(3))
            actual = found.group(1) if found else None
            if variant is not None:
                if actual != variant:
                    continue
            elif not matched(actual):
                continue
            hits.add(int(match.group(2), 16))
    if len(hits) > 1:
        raise SystemExit('ambiguous %s: %r' % (name, sorted(hits)))
    if hits:
        return hits.pop()
    raise SystemExit('missing ' + name)


def field(xml_dir, domain, register, bit):
    for filename in ('a6xx.xml', 'a6xx_gmu.xml'):
        root = ET.parse(xml_dir / filename).getroot()
        for entry in root.findall('r:domain', NS):
            if entry.attrib['name'] != domain:
                continue
            for reg in entry.findall('r:reg32', NS):
                if reg.attrib.get('name') != register:
                    continue
                for item in reg.findall('r:bitfield', NS):
                    if item.attrib.get('name') == bit:
                        return 1 << int(item.attrib['pos'])
    raise SystemExit('missing %s.%s' % (register, bit))


def derive_registers(source, windows):
    xml_dir = source / 'drivers/gpu/drm/msm/registers/adreno'
    gmu_source = (source / 'drivers/gpu/drm/msm/adreno/a6xx_gmu.c').read_text()
    regs = {}
    core = ['GMU_CM3_ITCM_START', 'GMU_CM3_DTCM_START', 'GMU_CM3_SYSRESET',
            'GMU_CM3_BOOT_CONFIG', 'GMU_CM3_FW_INIT_RESULT', 'GMU_CM3_CFG',
            'GMU_HFI_QTBL_ADDR', 'GMU_HFI_QTBL_INFO', 'GMU_HFI_CTRL_INIT',
            'GMU_HFI_CTRL_STATUS', 'GMU_HOST2GMU_INTR_SET', 'GMU_SYS_BUS_CONFIG',
            'GMU_ICACHE_CONFIG', 'GMU_DCACHE_CONFIG', 'GMU_AHB_FENCE_RANGE_0',
            'GMU_GMU2HOST_INTR_INFO', 'GMU_AO_HOST_INTERRUPT_STATUS']
    for name in core:
        regs['REG_' + name] = resolve(xml_dir, name, ['a6xx.xml', 'adreno_common.xml'])
    for name in ['GPU_GMU_CX_GMU_CX_FALNEXT_INTF', 'GPU_GMU_CX_GMU_CX_FAL_INTF']:
        # A6XX and A7XX share the 0x1f8f0 page; A8XX moved it.
        regs['REG_' + name] = resolve(xml_dir, name, ['a6xx.xml'], kind='reg32',
                                      variant='A6XX')
    for index in (8, 9, 10):
        regs['REG_GMU_GENERAL_%d' % index] = resolve(
            xml_dir, 'GMU_GENERAL_%d' % index, ['a6xx.xml'])
    rsc = ['GPU_RSCC_RSC_STATUS0_DRV0', 'RSCC_PDC_SLAVE_ID_DRV0',
           'RSCC_HIDDEN_TCS_CMD0_ADDR', 'RSCC_HIDDEN_TCS_CMD0_DATA',
           'RSCC_OVERRIDE_START_ADDR', 'RSCC_PDC_SEQ_START_ADDR',
           'RSCC_PDC_MATCH_VALUE_LO', 'RSCC_PDC_MATCH_VALUE_HI', 'RSCC_SEQ_BUSY_DRV0']
    for name in rsc:
        regs['REG_' + name] = resolve(xml_dir, name, ['a6xx.xml'])
    regs['REG_RSCC_SEQ_MEM_0_DRV0_A740'] = resolve(
        xml_dir, 'RSCC_SEQ_MEM_0_DRV0_A740', ['a6xx.xml'])
    regs['REG_CX_MISC_TCM_RET_CNTL'] = resolve(
        xml_dir, 'CX_MISC_TCM_RET_CNTL', ['a6xx.xml'])
    regs['REG_GBIF_QSB_SIDE0'] = resolve(xml_dir, 'GBIF_QSB_SIDE0', ['a6xx.xml'])
    for name in ('SEQ_START_ADDR', 'ENABLE_PDC'):
        regs['REG_PDC_GPU_' + name] = resolve(xml_dir, 'GPU_' + name, ['a6xx.xml'])
    fatal = sum(field(xml_dir, 'A6XX', 'GMU_AO_HOST_INTERRUPT_STATUS', bit)
                for bit in ('WDOG_BITE', 'FENCE_ERR', 'HOST_AHB_BUS_ERROR'))
    cm3 = field(xml_dir, 'A6XX', 'GMU_GMU2HOST_INTR_INFO', 'CM3_FAULT')

    init = gmu_source[gmu_source.index('static void a6xx_gmu_rpmh_init('):
                      gmu_source.index('/*\n * The lowest 16 bits')]

    def number(expression):
        expression = expression.strip()
        if '?' in expression:
            if not expression.startswith('adreno_is_a740_family(adreno_gpu)'):
                raise SystemExit('unrecognized conditional: ' + expression)
            expression = expression.split('?', 1)[1].split(':', 1)[0].strip()
        expression = re.sub(r'BIT\((\d+)\)', lambda m: str(1 << int(m[1])), expression)
        expression = expression.replace('seqmem0_drv0_reg',
                                        str(regs['REG_RSCC_SEQ_MEM_0_DRV0_A740']))
        for name, value in regs.items():
            original = name.replace('REG_', 'REG_A6XX_', 1)
            expression = re.sub(r'\b' + original + r'\b', str(value), expression)
        parsed = ast.parse(expression, mode='eval')
        if any(not isinstance(node, (ast.Expression, ast.Constant, ast.BinOp, ast.Add))
               for node in ast.walk(parsed)):
            raise SystemExit('unsupported source expression ' + expression)
        return eval(compile(parsed, '<pinned-source>', 'eval'), {'__builtins__': {}})

    setup = []
    prefix = init[init.index('/* Disable SDE clock gating */'):
                  init.index('/* The second spin')]
    for reg, value in re.findall(r'gmu_write_rscc\(gmu,\s*([^,]+),\s*([\s\S]*?)\);', prefix):
        setup.append((1, 4 * number(reg), number(value)))
    if 'seqmem0_drv0_reg = REG_A7XX_RSCC_SEQ_MEM_0_DRV0_A740;' not in init:
        raise SystemExit('A740 sequencer selection absent from the pinned source')
    sequence = init[init.index('gmu_write_rscc(gmu, seqmem0_drv0_reg,'):
                    init.index('} else {', init.index('gmu_write_rscc(gmu, seqmem0_drv0_reg,'))]
    for reg, value in re.findall(r'gmu_write_rscc\(gmu,\s*([^,]+),\s*([^;]+)\);', sequence):
        setup.append((1, 4 * number(reg), number(value)))
    tail = init[init.index('setup_pdc:'):]
    for reg, value in re.findall(r'pdc_write\(pdcptr,\s*([^,]+),\s*([^;]+)\);', tail):
        setup.append((2, 4 * number(reg), number(value)))
    if len(setup) != 19:
        raise SystemExit('A740 RSC/PDC sequence length differs: %d' % len(setup))
    for window, offset, value in setup:
        if offset & 3 or offset + 4 > windows[window]['bytes'] or not 0 <= value <= 0xffffffff:
            raise SystemExit('generated setup outside its window')
    return regs, setup, fatal, cm3


# --------------------------------------------------------------------- derive

def derive(args):
    dtb = args.dtb.resolve()
    source = args.source.resolve()
    if not dtb.is_file():
        raise SystemExit('%s is not a file' % dtb)
    if not (source / 'drivers/gpu/drm/msm/adreno/a6xx_gmu.c').is_file():
        raise SystemExit('%s does not hold the pinned Linux Adreno sources' % source)

    gpu_reg = named_ranges(dtb, GPU_NODE)
    gmu_reg = named_ranges(dtb, GMU_NODE)
    smmu_reg = ranges(dtb, SMMU_NODE)
    if not smmu_reg:
        raise SystemExit('SMMU node has no register window')
    windows = [
        gmu_reg['gmu'],
        gpu_reg['rscc'],
        gmu_reg['gmu_pdc'],
        gpu_reg['kgsl_3d0_reg_memory'],
        gpu_reg['cx_misc'],
    ]
    for window in windows:
        if not window['base'] or not window['bytes']:
            raise SystemExit('empty GPU/GMU window')
    regs, setup, fatal, cm3 = derive_registers(source, windows)

    gpu_node = GPU_NODE
    gmu_node = GMU_NODE
    chip_id = one(dtb, gpu_node, 'qcom,chipid')
    model = fdtget(dtb, gpu_node, 'qcom,gpu-model', 's').strip('"')
    compatible = text(dtb, gpu_node, 'compatible')

    # Stream IDs: the GPU node has no iommus on uke; its two child nodes and
    # the GMU node carry them. The first specifier cell is the SID.
    user_sid = cells(dtb, SMMU_NODE + '/gfx3d_user', 'iommus')[1]
    secure_sid = cells(dtb, SMMU_NODE + '/gfx3d_secure', 'iommus')[1]
    gmu_sid = cells(dtb, gmu_node, 'iommus')[1]

    bus = cells(dtb, gpu_node, 'qcom,bus-table-ddr')
    pwrlevels = []
    for child in children(dtb, gpu_node + '/qcom,gpu-pwrlevels'):
        node = gpu_node + '/qcom,gpu-pwrlevels/' + child
        frequency = one(dtb, node, 'qcom,gpu-freq')
        level = one(dtb, node, 'qcom,level')
        bus_index = one(dtb, node, 'qcom,bus-freq')
        acd = one(dtb, node, 'qcom,acd-level')
        if not frequency or not level or bus_index >= len(bus):
            raise SystemExit('invalid pwrlevel ' + child)
        pwrlevels.append(dict(hz=frequency, level=level, bus_index=bus_index,
                              kbps=bus[bus_index], acd_present=True, acd=acd))
    pwrlevels.sort(key=lambda entry: entry['hz'])
    if len(pwrlevels) != 9 or any(pwrlevels[i]['level'] <= pwrlevels[i - 1]['level']
                                  for i in range(1, len(pwrlevels))):
        raise SystemExit('GPU pwrlevels are not 9 ascending OPPs')
    gpu_opps = [dict(hz=0, level=0, kbps=0, acd_present=0, acd=0)] + pwrlevels
    # Level 0 carries no bandwidth vote: AdrenoHfiBuildBandwidth requires kbps[0]==0.
    bandwidths = [0] + sorted({entry['kbps'] for entry in pwrlevels if entry['kbps']})
    if len(bandwidths) < 2 or bandwidths[0] != 0 or any(
            bandwidths[i] <= bandwidths[i - 1] for i in range(1, len(bandwidths))):
        raise SystemExit('GPU bandwidth table is not a rising list with a zero level')

    gmu_freqs = cells(dtb, gmu_node, 'qcom,gmu-freq-table')
    if len(gmu_freqs) % 2:
        raise SystemExit('GMU frequency table is not {hz, level} pairs')
    gmu_opps = [dict(hz=0, level=0, kbps=0, acd_present=0, acd=0)]
    gmu_opps += [dict(hz=gmu_freqs[i], level=gmu_freqs[i + 1], kbps=0,
                      acd_present=0, acd=0)
                 for i in range(0, len(gmu_freqs), 2)]
    gmu_opps.sort(key=lambda entry: entry['hz'])
    if len(gmu_opps) < 2 or any(gmu_opps[i]['level'] <= gmu_opps[i - 1]['level']
                                for i in range(2, len(gmu_opps))):
        raise SystemExit('GMU OPPs are not ascending')

    cmd_db = reserved_with_compatible(dtb, 'qcom,cmd-db')
    cmd_reg = ranges(dtb, cmd_db)[0]
    zap_phandle = one(dtb, gpu_node + '/zap-shader', 'memory-region')
    zap = reserved_with_phandle(dtb, zap_phandle)
    zap_reg = ranges(dtb, zap)[0]
    gpucc = soc_node_with_compatible(dtb, 'qcom,cliffs-gpucc')
    gcc = soc_node_with_compatible(dtb, 'qcom,cliffs-gcc')

    # The device repository's board.toml is the source of truth when the
    # derive mode runs there; the recorded section makes the build tree
    # self-contained (the tree has scripts/ but no board.toml).
    gpu_board = device_gpu()
    board = dict(
        chip_id=gpu_board.get('chip_id', hex(chip_id)),
        model=gpu_board.get('model', model),
        gmem_bytes=gpu_board.get('gmem_bytes', '0x180000'),
        gmu_chip_id=gpu_board.get('gmu_chip_id', '0x70b0000'),
        gmu_cgc_mode=gpu_board.get('gmu_cgc_mode', '0x00020202'),
        preempt_record_bytes=gpu_board.get('preempt_record_bytes', '0x225000'),
        gmu_pool_base=gpu_board.get('gmu_pool_base', '0xc8000000'),
        gmu_pool_bytes=gpu_board.get('gmu_pool_bytes', '0x2000000'),
        ubwc_version=gpu_board.get('ubwc_version', 4),
        ubwc_swizzle=gpu_board.get('ubwc_swizzle', 3),
        highest_bank_bit=gpu_board.get('highest_bank_bit', 16),
        gpu_base=hex(windows[3]['base']), gpu_size=hex(windows[3]['bytes']),
        gmu_base=hex(windows[0]['base']), gmu_size=hex(windows[0]['bytes']),
        gmu_pdc_base=hex(windows[2]['base']), gmu_pdc_size=hex(windows[2]['bytes']),
        gpucc_base=hex(ranges(dtb, gpucc)[0]['base']),
        gcc_base=hex(ranges(dtb, gcc)[0]['base']),
        sid_user=user_sid, sid_secure=secure_sid, sid_gmu=gmu_sid,
    )

    # A732 catalog values: MCC45TR/senemos-uke-kernel-mainline patch
    # 0036-uke-adreno732-render.patch (public), which cites the inspected
    # LineageOS SM8635 modules. Preemption and IFPC stay unqualified.
    data = dict(
        format_version=1,
        board=board,
        chip=dict(chip_id=hex(chip_id), model=model,
                  compatible=next((item for item in compatible
                                   if item.startswith('qcom,adreno')), compatible[0]),
                  family='ADRENO_7XX_GEN2'),
        dt=dict(
            dtb_sha256=sha(dtb),
            gpu_node=gpu_node, gmu_node=gmu_node, smmu_node=SMMU_NODE,
            gpu_reg_names=list(gpu_reg), gmu_reg_names=list(gmu_reg),
            windows=[dict(name=name, base=window['base'], bytes=window['bytes'])
                     for name, window in zip(
                         ('gmu', 'rscc', 'gmu_pdc', 'kgsl_3d0_reg_memory', 'cx_misc'),
                         windows)],
            streams=dict(user=user_sid, secure=secure_sid, gmu=gmu_sid),
            gpu_opps=gpu_opps, gmu_opps=gmu_opps,
            bandwidths_kbps=bandwidths,
            command_db=dict(node=cmd_db, base=cmd_reg['base'], bytes=cmd_reg['bytes']),
            zap=dict(node=zap, base=zap_reg['base'], bytes=zap_reg['bytes']),
            gpucc=dict(node=gpucc, base=ranges(dtb, gpucc)[0]['base'],
                       bytes=ranges(dtb, gpucc)[0]['bytes']),
            gcc=dict(node=gcc, base=ranges(dtb, gcc)[0]['base'],
                     bytes=ranges(dtb, gcc)[0]['bytes']),
        ),
        catalog=dict(
            source='MCC45TR/senemos-uke-kernel-mainline 0036-uke-adreno732-render.patch',
            gpu_chip_id=board['chip_id'],
            gmu_chip_id=board['gmu_chip_id'],
            gmu_cgc_mode=board['gmu_cgc_mode'],
            gmem_bytes=board['gmem_bytes'],
            preempt_record_bytes=board['preempt_record_bytes'],
            quirks=dict(cached_coherent=True, hw_apriv=True,
                        preemption=False, ifpc=False),
            bcms=[dict(name='SH0', buswidth=16, fixed=0, perfmode=0, perfmode_kbps=0),
                  dict(name='MC0', buswidth=4, fixed=0, perfmode=0, perfmode_kbps=0),
                  dict(name='ACV', buswidth=0, fixed=1, perfmode=4, perfmode_kbps=0)],
            ubwc=dict(enc_version=board['ubwc_version'], dec_version=board['ubwc_version'],
                      swizzle=board['ubwc_swizzle'], bank_spread=True,
                      highest_bank_bit=board['highest_bank_bit'], macrotile=True),
        ),
        registers=dict(
            source='pinned Linux 42f3b40c a6xx_gmu.c / a6xx.xml / a6xx_gmu.xml',
            defines=regs, rsc_setup=setup,
            core_gpu_delta=windows[0]['base'] - windows[3]['base'],
            ao_fatal_mask=fatal, cm3_fatal_mask=cm3,
        ),
        firmware=dict(
            sqe=dict(name='gen71100_sqe.fw', bytes=66036,
                     sha256='b6df660d109504dbea5a266910b037a33d893a11c6de52f524a5fd112008c401'),
            gmu=dict(name='gen71100_gmu.bin', bytes=80680,
                     sha256='435c7bac9a328b8a100df930b84b65b4d579e42be44785d0e60e3729291bb864'),
            zap=dict(name='gen71100_zap.mbn', bytes=12088,
                     sha256='1294992888e8c7c7fcd8a6d0a672cc666eff3c95e666ce5840db7e03edcc500d'),
        ),
        provenance=dict(
            dt='live uke DT dump (private physical evidence, hashes only)',
            register_map='pinned ianchb/sm8550-mainline 42f3b40c',
            catalog='public A732 patch; no device run',
            hardware_registers_observed=False,
        ),
    )
    DATA.parent.mkdir(parents=True, exist_ok=True)
    DATA.write_text(json.dumps(data, indent=2) + '\n')
    print('PASS: derived A732 inputs from %s (%d-byte DTB, %d GPU OPPs, %d bandwidths)'
          % (dtb.name, dtb.stat().st_size, len(gpu_opps), len(bandwidths)))


# ---------------------------------------------------------------------- build

def load_inputs():
    if not DATA.is_file():
        raise SystemExit('%s is missing; run prepare-uke-gmu.py --derive first' % DATA)
    return json.loads(DATA.read_text())


def device_gpu():
    """The device repository's board.toml, when the derive mode runs there."""
    path = ROOT / 'board.toml'
    if not path.is_file():
        return {}
    try:
        import tomllib
    except ModuleNotFoundError:
        raise SystemExit('Python 3.11+ is required to read board.toml')
    return tomllib.loads(path.read_text()).get('gpu', {})


def recorded_board(data):
    board = data.get('board')
    if not board:
        raise SystemExit('recorded inputs lack the board section; re-run --derive')
    return board


def verify_firmware(data):
    checked = {}
    for role in ('sqe', 'gmu', 'zap'):
        pin = data['firmware'][role]
        name = pin['name']
        path = FIRMWARE / name
        if path.is_file():
            blob = path.read_bytes()
            if len(blob) != pin['bytes'] or sha(path) != pin['sha256']:
                raise SystemExit('firmware %s does not match its pin' % name)
            checked[role] = dict(name=name, bytes=len(blob), sha256=pin['sha256'],
                                 present=True)
        else:
            checked[role] = dict(name=name, bytes=pin['bytes'], sha256=pin['sha256'],
                                 present=False)
    return checked


def build(args):
    if os.environ.get('XNU_NATIVE_ADRENO', '0') != '1' and not args.force:
        print('SKIP: NATIVE_ADRENO is off for this configuration')
        return
    data = load_inputs()
    board = recorded_board(data)
    checks = [
        ('chip_id', data['chip']['chip_id'], board['chip_id']),
        ('model', data['chip']['model'], board['model']),
        ('gmem', data['catalog']['gmem_bytes'], board['gmem_bytes']),
        ('gmu_chip_id', data['catalog']['gmu_chip_id'], board['gmu_chip_id']),
        ('gmu_cgc_mode', data['catalog']['gmu_cgc_mode'], board['gmu_cgc_mode']),
        ('preempt_record', data['catalog']['preempt_record_bytes'],
         board['preempt_record_bytes']),
    ]
    for what, recorded, target in checks:
        if what == 'model':
            if str(recorded) != str(target):
                raise SystemExit('%s: recorded %s != board %s' % (what, recorded, target))
            continue
        if int(str(recorded), 0) != int(str(target), 0):
            raise SystemExit('%s: recorded %s != board %s' % (what, recorded, target))
    firmware = verify_firmware(data)
    gmu_pin = firmware['gmu']
    regs = data['registers']
    windows = data['dt']['windows']
    if len(windows) != 5 or len(regs['rsc_setup']) != 19:
        raise SystemExit('recorded window/RSC layout is incomplete')

    OUT.mkdir(parents=True, exist_ok=True)
    # The reserved GMU pool is board-owned physical memory (the ADT's gmu-memory node and the UEFI
    # "GMU Pool" map entry), adopted by the driver instead of allocated from the kernel VM. The
    # build tree keeps the recorded fallback so it stays self-contained.
    pool_board = device_gpu()
    pool_base = pool_board.get('gmu_pool_base', board.get('gmu_pool_base', '0xc8000000'))
    pool_bytes = pool_board.get('gmu_pool_bytes', board.get('gmu_pool_bytes', '0x2000000'))
    if int(str(pool_bytes), 0) < 0x1412000 or int(str(pool_base), 0) & 0xfff:
        raise SystemExit('gmu_pool must be page aligned and hold the 0x1412000 payload bytes')
    defines = '\n'.join('#define %s UINT32_C(%s)' % (key, hex(value))
                        for key, value in regs['defines'].items())
    source = ['/* Generated by scripts/prepare-uke-gmu.py. Do not edit. */',
              '#include <stdint.h>', '#include <stddef.h>', defines,
              '#define SHENG_GMU_CHIP_ID UINT32_C(%s)' % data['chip']['chip_id'],
              '#define SHENG_GMU_FW_CHIP_ID UINT32_C(%s)' % data['catalog']['gmu_chip_id'],
              '#define SHENG_GMU_CORE_GPU_DELTA %su' % hex(regs['core_gpu_delta']),
              '#define SHENG_GMU_FIRMWARE_BYTES %du' % gmu_pin['bytes'],
              '#define SHENG_GMU_AO_FATAL_MASK %su' % hex(regs['ao_fatal_mask']),
              '#define SHENG_GMU_CM3_FATAL_MASK %su' % hex(regs['cm3_fatal_mask']),
              '#define SHENG_GMU_POOL_BASE UINT64_C(%s)' % hex(int(str(pool_base), 0)),
              '#define SHENG_GMU_POOL_BYTES %du' % int(str(pool_bytes), 0),
              'static const uint8_t sheng_gmu_firmware_sha256[32]={%s};' %
              ','.join('0x' + gmu_pin['sha256'][i:i + 2] for i in range(0, 64, 2)),
              'static const uint64_t sheng_gmu_window_physical[5]={%s};' %
              ','.join(hex(window['base']) for window in windows),
              'static const size_t sheng_gmu_window_bytes[5]={%s};' %
              ','.join(hex(window['bytes']) for window in windows),
              'typedef struct {unsigned window;size_t byte_offset;uint32_t value;} ShengGmuSetupWrite;',
              '#define SHENG_GMU_RSC_SETUP_COUNT 19u',
              'static const ShengGmuSetupWrite sheng_gmu_rsc_setup[19]={']
    source += ['{%d,%s,%s},' % (window, hex(offset), hex(value))
               for window, offset, value in regs['rsc_setup']]
    source.append('};')
    (OUT / 'ShengGmuSource.h').write_text('\n'.join(source) + '\n')

    votes = ['/* Generated by scripts/prepare-uke-gmu.py. Do not edit. */',
             '#include <stdint.h>',
             '#define SHENG_GMU_VOTES_CHIP_ID %su' % data['chip']['chip_id'],
             'typedef struct {uint64_t hz;uint32_t level,kbps,acd_present,acd;} ShengGmuOpp;']
    for kind in ('gpu', 'gmu'):
        entries = data['dt'][kind + '_opps']
        votes.append('#define SHENG_%s_OPPS %du' % (kind.upper(), len(entries)))
        votes.append('static const ShengGmuOpp sheng_%s_opps[]={' % kind)
        votes += ['{%d,%d,%d,%d,%d},' % (entry['hz'], entry['level'], entry['kbps'],
                                         entry['acd_present'], entry['acd'])
                  for entry in entries]
        votes.append('};')
    votes.append('#define SHENG_GPU_BANDWIDTHS %du' % len(data['dt']['bandwidths_kbps']))
    votes.append('static const uint32_t sheng_gpu_bandwidths[]={%s};' %
                 ','.join(str(value) for value in data['dt']['bandwidths_kbps']))
    votes.append('static const AdrenoHfiBcm sheng_bcm_policy[3]={')
    votes += ['{.buswidth=%d,.fixed=%d,.perfmode=%d,.perfmode_kbps=%d},' %
              (entry['buswidth'], entry['fixed'], entry['perfmode'], entry['perfmode_kbps'])
              for entry in data['catalog']['bcms']]
    votes.append('};')
    (OUT / 'ShengGmuVotesSource.h').write_text('\n'.join(votes) + '\n')

    command_db = data['dt']['command_db']
    (OUT / 'ShengGmuCommandDbSource.h').write_text(
        '/* Generated by scripts/prepare-uke-gmu.py. Do not edit. */\n'
        '#define SHENG_GMU_COMMAND_DB_BASE UINT64_C(%s)\n'
        '#define SHENG_GMU_COMMAND_DB_BYTES %du\n' %
        (hex(command_db['base']), command_db['bytes']))

    report = dict(chip=data['chip'], firmware=firmware, gpu_opps=len(data['dt']['gpu_opps']),
                  gmu_opps=len(data['dt']['gmu_opps']),
                  bandwidths=len(data['dt']['bandwidths_kbps']),
                  windows=windows, rsc_writes=len(regs['rsc_setup']),
                  source_sha256={name: sha(OUT / name) for name in
                                 ('ShengGmuSource.h', 'ShengGmuVotesSource.h',
                                  'ShengGmuCommandDbSource.h')},
                  hardware_ready=False, hardware_registers_observed=False)
    (OUT / 'uke-gpu-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: A732 headers for %s (%d GPU OPPs, %d bandwidths, %d RSC writes); '
          'firmware %s' % (data['chip']['model'], len(data['dt']['gpu_opps']),
                           len(data['dt']['bandwidths_kbps']), len(regs['rsc_setup']),
                           'present' if gmu_pin['present'] else 'absent (weak symbols)'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--derive', action='store_true')
    parser.add_argument('--dtb', type=Path, default=ROOT / 'build/adreno-firmware/uke.dtb')
    parser.add_argument('--source', type=Path, default=ROOT / '.cache/adreno-source')
    parser.add_argument('--force', action='store_true',
                        help='build headers even when NATIVE_ADRENO is off')
    args = parser.parse_args()
    if args.derive:
        derive(args)
    else:
        build(args)


if __name__ == '__main__':
    main()
