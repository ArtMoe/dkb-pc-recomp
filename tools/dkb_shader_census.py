"""Census of the NVN 2 (Ampere) shader stages in the extracted Bananza RomFS, written as one seed pack.

Every RomFS file is read, zstd frames are decoded (no dictionary pack is needed) and SARC archives are opened. Each BNSH
inside (a BFSHA, an effect set, a layout archive) lists its programs, and each program its vertex, pixel and compute stage
as a control section and its code. A stage is named the way the runtime names it (runtime/gfx_shader.c,
gfx_shader_nvn2_hash: FNV-1a over the control section and the code), which is the DKB_SHADER_DUMP file name, so the pack
can be compared with a dump of what the game loaded.

Writes build/<version>/shaders-nvn2/seed-nvn2.bin, read by `dkb.exe --prepare-shaders`:

    "DKBN2SD1", u32 count, then per stage: u32 stage (GFX_STAGE_*: 0 vertex, 4 pixel, 5 compute), u32 control size,
    u32 code size, the control section and the code, each padded to 4 bytes.

Not covered yet: the agl shader packages (BAHS) in Lib/agl_resource, the executable modules and the sead primitive
drawer's bare shader file. They hold about 3% of the stages the game loads; the runtime still translates those on first
use. Game content: the pack stays under build/ (ignored).

    .venv\\Scripts\\python tools\\dkb_shader_census.py --version dkb100
"""
import argparse
import collections
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

try:
    import compression.zstd as _zstd

    def zstd_decompress(raw):
        return _zstd.decompress(raw)
except ImportError:   # Python before 3.14
    import zstandard as _zstd

    def zstd_decompress(raw):
        return _zstd.ZstdDecompressor().decompress(raw, max_output_size=1 << 30)

ROOT = Path(__file__).resolve().parent.parent
ZSTD = b'\x28\xb5\x2f\xfd'
CONTROL_MAGIC = bytes.fromhex('34127698c9000000')   # 0x98761234, version 0xC9
BNSH = b'BNSH' + bytes(4)
STAGE_IDS = {'vs': 0, 'ps': 4, 'cs': 5}              # GFX_STAGE_* in runtime/gfx.h
MAX_FILE = 600 << 20


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('structure outside the file')
    return data[offset:offset + size]


def sarc(data):
    """Name -> bytes of a SARC archive (as tools/shader_census.py reads one)."""
    if data[:4] != b'SARC':
        raise ValueError('expected SARC')
    endian = '<' if data[6:8] == bytes([0xff, 0xfe]) else '>'
    header, size, start = struct.unpack_from(endian + 'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from(endian + 'H', data, header + 6)[0]
    names = header + 12 + count * 16
    if checked(data, names, 4) != b'SFNT':
        raise ValueError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian + '4I', checked(data, header + 12 + i * 16, 16))
        off = names + (attribute & 0xffffff) * 4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo:
            raise ValueError('invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        result.setdefault(name, checked(data, start + lo, hi - lo))
    return result


def fnv1a(blob):
    h = 0xcbf29ce484222325
    for b in blob:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def stage_sizes(control):
    """The control size and code size the runtime hashes (runtime/gfx_shader.c, nvn2_sizes)."""
    ctl_size, = struct.unpack_from('<I', control, 0x30)
    length, = struct.unpack_from('<I', control, 0x38)
    c1_end, = struct.unpack_from('<I', control, 0x44)
    ctl_size = ctl_size or 0xd8
    if c1_end > length and c1_end - length <= 0x10000:
        length = c1_end
    return ctl_size, length


def bnsh_stages(data):
    """Yield (stage, control, code) for every vertex, pixel and compute stage of one BNSH file."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe':
        return
    size, = struct.unpack_from('<I', data, 28)
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count * 64)
    for i in range(count):
        program, = struct.unpack_from('<Q', data, variations + i * 64 + 16)
        checked(data, program, 56)
        for stage, relative in (('vs', 8), ('ps', 40), ('cs', 48)):
            offset, = struct.unpack_from('<Q', data, program + relative)
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset + 8, 24))
            control = checked(data, control_off, control_size)
            if control[:8] != CONTROL_MAGIC:
                continue
            ctl_size, length = stage_sizes(control)
            code = checked(data, code_off, code_size)
            if ctl_size > len(control) or not length or length > len(code):
                continue
            yield stage, control[:ctl_size], code[:length]


def walk(data, name, found, errors):
    if data[:4] == ZSTD:
        data = zstd_decompress(data)
    if data[:4] == b'SARC':
        for member, raw in sarc(data).items():
            walk(raw, name + '/' + member, found, errors)
        return
    pos = 0
    while (pos := data.find(BNSH, pos)) >= 0:
        size, = struct.unpack_from('<I', checked(data, pos + 28, 4))
        try:
            for stage, control, code in bnsh_stages(checked(data, pos, size)):
                found.setdefault(fnv1a(control + code), (stage, control, code, name))
        except (ValueError, struct.error) as e:
            errors[str(e)] += 1
        pos += max(size, 8)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--version', default='dkb100')
    ap.add_argument('--romfs', type=Path, help='folder with romfs.bin and index.json (default build/extracted/<version>/romfs)')
    ap.add_argument('--out', type=Path, help='output folder (default build/<version>/shaders-nvn2)')
    args = ap.parse_args()
    romfs = args.romfs or ROOT / 'build' / 'extracted' / args.version / 'romfs'
    out = args.out or ROOT / 'build' / args.version / 'shaders-nvn2'
    index = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if index.get('status') != 'pass':
        sys.exit('RomFS has not passed integrity verification')
    found, errors, t0 = {}, collections.Counter(), time.time()
    with (romfs / 'romfs.bin').open('rb') as stream:
        for row in index['files']:
            if row['size'] > MAX_FILE:
                continue
            stream.seek(row['offset'])
            raw = stream.read(row['size'])
            if raw[:4] not in (ZSTD, b'SARC', b'FSHA', b'BNSH') and BNSH not in raw:
                continue
            try:
                walk(raw, row['path'], found, errors)
            except (ValueError, struct.error, OSError, _zstd.ZstdError) as e:
                errors[type(e).__name__ + ': ' + str(e)[:50]] += 1
    out.mkdir(parents=True, exist_ok=True)
    pad = lambda b: b + bytes(-len(b) % 4)
    by_stage = collections.Counter(v[0] for v in found.values())
    blob = bytearray(b'DKBN2SD1' + struct.pack('<I', len(found)))
    for key in sorted(found):
        stage, control, code, _ = found[key]
        blob += struct.pack('<III', STAGE_IDS[stage], len(control), len(code)) + pad(control) + pad(code)
    (out / 'seed-nvn2.bin').write_bytes(blob)
    (out / 'index.json').write_text(json.dumps({format(k, '016x'): [v[0], v[3]] for k, v in sorted(found.items())}, indent=0))
    print(f'{len(found)} stages {dict(by_stage)} in {time.time() - t0:.0f} s, {len(blob) >> 20} MiB -> {out / "seed-nvn2.bin"}')
    if errors:
        print('skipped:', errors.most_common(5))


if __name__ == '__main__':
    main()
