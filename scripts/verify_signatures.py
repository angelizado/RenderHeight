import argparse
import hashlib
import struct
from pathlib import Path

EXPECTED_SHA256 = "ebfd5d43a165dcec5d4007b9d41ed5b2ed122fcd02ecfef7e4ed35b40826821f"
EXPECTED_BASE = 0x140000000
EXPECTED_RVAS = {"options": 0x10990, "vector_insert": 0x23590}
SIGNATURES = {
    "options": "48 89 5c 24 20 56 57 41 56 48 83 ec 60 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 50 "
                "48 8b c2 48 8b d9 48 89 4c 24 30 33 ff 89 7c 24 24 49 8b d0 48 8b c8 e8 ?? ?? ?? ?? "
                "44 8b f0 b8 e0 10 00 00 45 85 f6 74 0b 43 8d 34 36 3b f0 0f 47 f0 eb 05 be 70 08 00 00 "
                "48 89 3b 48 89 7b 08 48 89 7b 10",
    "vector_insert": "48 89 6c 24 20 57 41 54 41 56 48 83 ec 20 48 8b 41 08 48 8b ea 48 8b 11 4c 8b f5 4c 2b f2 "
                     "48 2b c2 4d 8b e0 49 c1 fe 02 48 c1 f8 02 49 b8 ff ff ff ff ff ff ff 3f 48 8b f9 49 3b c0 "
                     "0f 84 c7 00 00 00 48 8b 49 10 48 2b ca 48 89 5c 24 40 48 c1 f9 02 48 8b d1 4c 89 7c 24 50 "
                     "4c 8d 78 01 48 d1 ea 49 8b c0 48 2b c2 48 3b c8 76 38 49 8b d8 48 8d 0c 9d 00 00 00 00 "
                     "48 89 74 24 48 e8 ef ae fe ff",
}


def parse_pattern(text):
    values = []
    mask = []
    for token in text.split():
        if token in ("?", "??"):
            values.append(0)
            mask.append(False)
        else:
            values.append(int(token, 16))
            mask.append(True)
    return bytes(values), mask


def parse_pe(data):
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("bad PE signature")
    machine, section_count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic != 0x20B:
        raise ValueError(f"expected PE32+, got optional-header magic {magic:#x}")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    section_table = optional + optional_size
    sections = []
    for i in range(section_count):
        off = section_table + i * 40
        name = data[off:off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, off + 8)
        characteristics = struct.unpack_from("<I", data, off + 36)[0]
        if characteristics & 0x20000000:
            sections.append((name, rva, raw_size, raw_offset, virtual_size))
    return machine, image_base, sections


def scan(data, sections, text):
    values, mask = parse_pattern(text)
    found = []
    for name, rva, raw_size, raw_offset, _ in sections:
        region = data[raw_offset:raw_offset + raw_size]
        if len(region) < len(values):
            continue
        for offset in range(len(region) - len(values) + 1):
            if all(not check or region[offset + i] == values[i] for i, check in enumerate(mask)):
                found.append((name, rva + offset))
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("azimuth", type=Path)
    path = parser.parse_args().azimuth
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"SHA-256 mismatch: {digest}")
    machine, image_base, sections = parse_pe(data)
    if machine != 0x8664 or image_base != EXPECTED_BASE:
        raise SystemExit(f"unexpected PE identity: machine={machine:#x}, imagebase={image_base:#x}")
    for key, signature in SIGNATURES.items():
        matches = scan(data, sections, signature)
        if len(matches) != 1 or matches[0][1] != EXPECTED_RVAS[key]:
            raise SystemExit(f"{key}: expected one match at RVA {EXPECTED_RVAS[key]:#x}; got {matches}")
        print(f"{key}: unique match at RVA {matches[0][1]:#x} in {matches[0][0]}")
    print(f"SHA-256 verified: {digest}")


if __name__ == "__main__":
    main()
