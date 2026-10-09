# SPDX-License-Identifier: MIT

"""Load a GBA cartridge image from a .gba ROM or a GSF rip (.gsflib/.minigsf/.gsf)."""
import struct
import zlib
from pathlib import Path

ROM_BASE = 0x08000000


def _psf_tags(data, pos):
    tags = {}
    if data[pos:pos + 5] != b'[TAG]':
        return tags
    for line in data[pos + 5:].decode('utf-8', 'surrogateescape').split('\n'):
        if '=' in line:
            key, value = line.split('=', 1)
            tags[key.strip().lower()] = value.strip()
    return tags


def _load_gsf(path, image, depth=0):
    if depth > 10:
        raise ValueError('GSF _lib chain too deep')
    data = path.read_bytes()
    if len(data) < 16 or data[:4] != b'PSF\x22':
        raise ValueError('%s isn\'t a GSF file' % path)
    reserved, size = struct.unpack('<II', data[4:12])
    program_pos = 16 + reserved
    if program_pos + size > len(data):
        raise ValueError('%s is truncated' % path)
    tags = _psf_tags(data, program_pos + size)
    # _lib first, then the file's program on top of it, then _lib2, _lib3 and so on up to the first one missing.
    if tags.get('_lib'):
        _load_gsf(path.parent / tags['_lib'], image, depth + 1)
    if size:
        program = zlib.decompress(data[program_pos:program_pos + size])
        if len(program) < 12:
            raise ValueError('the program section of %s is too short' % path)
        offset, length = struct.unpack('<II', program[4:12])
        start = offset & 0x01FFFFFF
        chunk = program[12:12 + length]
        if len(image) < start + len(chunk):
            image.extend(bytes(start + len(chunk) - len(image)))
        image[start:start + len(chunk)] = chunk
    n = 2
    while tags.get('_lib%d' % n):
        _load_gsf(path.parent / tags['_lib%d' % n], image, depth + 1)
        n += 1


def load_rom(path):
    """Returns the cartridge image (mapped at 0x08000000) as bytes."""
    data = Path(path).read_bytes()
    if data[:3] == b'PSF':
        image = bytearray()
        _load_gsf(Path(path), image)
        return bytes(image)
    return data[:0x2000000]
