#!/usr/bin/env python3
"""Check pinned stock WPSS pieces with the exact imported runtime MDT parser."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VERIFY = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "QcomMdt.h"
static QcomMdtSplit split;
static FILE *pieces[QCOM_MDT_MAX_SEGMENTS];
static int piece_read(void *unused, unsigned piece, uint64_t at, void *to, size_t n) {
    (void)unused;
    return !pieces[piece] || fseek(pieces[piece], (long)at, SEEK_SET) || fread(to, 1, n, pieces[piece]) != n;
}
static int read_joined(void *unused, uint64_t at, void *to, size_t n) {
    (void)unused;
    return qcom_mdt_split_read(&split, piece_read, NULL, at, to, n);
}
#define REQUIRE(x) do {if (!(x)) {fprintf(stderr, "WPSS check failed at line %d: %s\n", __LINE__, #x); return 1;}} while (0)
int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    char path[4096];
    unsigned char headers[4096];
    snprintf(path, sizeof path, "%s/wpss.b00", argv[1]);
    FILE *h = fopen(path, "rb"); REQUIRE(h);
    size_t bytes = fread(headers, 1, sizeof headers, h); fclose(h);
    REQUIRE(qcom_mdt_split(&split, headers, bytes) == QcomMdtOk);
    for (unsigned i = 0; i < split.count; ++i) {
        if (!split.piece[i].size) continue;
        snprintf(path, sizeof path, "%s/wpss.b%02u", argv[1], i);
        pieces[i] = fopen(path, "rb"); REQUIRE(pieces[i]);
    }
    QcomMdtImage image;
    REQUIRE(qcom_mdt_parse(&image, split.size, read_joined, NULL) == QcomMdtOk);
    unsigned count = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < image.count; ++i) {
        if (!image.segment[i].load) continue;
        uint64_t at = 0;
        REQUIRE(qcom_mdt_place(&image, i, 0x9bb00000, 0x1900000, &at) == QcomMdtOk);
        for (unsigned j = 0; j < i; ++j) {
            if (!image.segment[j].load) continue;
            uint64_t before = 0;
            REQUIRE(qcom_mdt_place(&image, j, 0x9bb00000, 0x1900000, &before) == QcomMdtOk);
            REQUIRE(at >= before + image.segment[j].memory_size || before >= at + image.segment[i].memory_size);
        }
        count++; total += image.segment[i].file_size;
    }
    size_t metadata_size = qcom_mdt_metadata_size(&image);
    unsigned char *metadata = malloc(metadata_size), *original = malloc(metadata_size + 1);
    REQUIRE(metadata && original);
    REQUIRE(qcom_mdt_metadata(&image, read_joined, NULL, metadata, metadata_size) == QcomMdtOk);
    snprintf(path, sizeof path, "%s/wpss.mdt", argv[1]);
    h = fopen(path, "rb"); REQUIRE(h);
    REQUIRE(fread(original, 1, metadata_size + 1, h) == metadata_size); fclose(h);
    REQUIRE(memcmp(metadata, original, metadata_size) == 0);
    REQUIRE(qcom_mdt_span(&image) == 0x1900000);
    unsigned char sentinel;
    REQUIRE(read_joined(NULL, split.size, &sentinel, 1) == QcomMdtOutside);
    printf("WPSS ELF%u: %u load segments, %llu file bytes, span %llu, metadata %zu matches stock MDT\n",
        image.elf64 ? 64 : 32, count, (unsigned long long)total,
        (unsigned long long)qcom_mdt_span(&image), metadata_size);
    free(metadata); free(original);
    for (unsigned i = 0; i < split.count; ++i) if (pieces[i]) fclose(pieces[i]);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, default=ROOT / '.cache/firmware/uke-wifi/qca6750')
    parser.add_argument('--protocol-dir', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'firmware/wcn6750/manifest.json').read_text())
    for name, pin in manifest['files'].items():
        data = (args.firmware / name).read_bytes()
        if len(data) != pin['size'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise SystemExit(f'firmware pin mismatch: {name}')
    provenance = json.loads((args.protocol_dir / 'upstream.json').read_text())
    for name, digest in provenance['files'].items():
        if hashlib.sha256((args.protocol_dir / name).read_bytes()).hexdigest() != digest:
            raise SystemExit(f'upstream source mismatch: {name}')
    with tempfile.TemporaryDirectory(prefix='uke-wpss-check-') as temporary:
        temporary = Path(temporary)
        source, executable = temporary / 'check.c', temporary / 'check'
        source.write_text(VERIFY)
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I', str(args.protocol_dir), str(source), str(args.protocol_dir / 'QcomMdt.c'),
                        '-o', str(executable)], check=True)
        subprocess.run([str(executable), str(args.firmware.resolve())], check=True)
    print(f"PASS: {len(manifest['files'])} firmware pins and {len(provenance['files'])} unchanged upstream protocol files")


if __name__ == '__main__':
    main()
