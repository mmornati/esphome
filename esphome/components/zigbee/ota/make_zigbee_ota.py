#!/usr/bin/env python3
"""Wrap an ESP-IDF app image into a Zigbee OTA Upgrade file (ZCL spec 11.4).

The file is a 56-byte header followed by one sub-element that holds the app
image, either as is (tag 0x0000, Upgrade Image) or zlib-compressed (tag
0xF100, manufacturer specific), which the zigbee OTA platform inflates on the
device. Standard library only: CMake runs it with ESP-IDF's Python.
"""

import argparse
from pathlib import Path
import struct
import zlib

FILE_IDENTIFIER = 0x0BEEF11E
HEADER_VERSION = 0x0100
HEADER_LENGTH = 56
STACK_VERSION_ZIGBEE_PRO = 0x0002
TAG_UPGRADE_IMAGE = 0x0000
TAG_IMAGE_ZLIB = 0xF100


def build(
    image: bytes,
    manufacturer_code: int,
    image_type: int,
    file_version: int,
    header_string: str,
    compress: bool,
) -> bytes:
    if compress:
        tag, payload = TAG_IMAGE_ZLIB, zlib.compress(image, 9)
    else:
        tag, payload = TAG_UPGRADE_IMAGE, image
    element = struct.pack("<HI", tag, len(payload)) + payload
    total_size = HEADER_LENGTH + len(element)
    header = struct.pack(
        "<IHHHHHIH32sI",
        FILE_IDENTIFIER,
        HEADER_VERSION,
        HEADER_LENGTH,
        0,  # field control: no optional fields
        manufacturer_code,
        image_type,
        file_version,
        STACK_VERSION_ZIGBEE_PRO,
        header_string.encode("ascii", "ignore")[:32].ljust(32, b"\0"),
        total_size,
    )
    assert len(header) == HEADER_LENGTH
    return header + element


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="app image (<name>.bin)")
    parser.add_argument("output", type=Path, help="Zigbee OTA file to write")
    parser.add_argument("--manufacturer-code", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--image-type", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--file-version", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--header-string", default="")
    parser.add_argument("--no-compress", action="store_true")
    args = parser.parse_args()

    image = args.image.read_bytes()
    data = build(
        image,
        args.manufacturer_code,
        args.image_type,
        args.file_version,
        args.header_string,
        not args.no_compress,
    )
    args.output.write_bytes(data)
    print(
        f"Zigbee OTA file {args.output.name}: version 0x{args.file_version:08X}, "
        f"manufacturer 0x{args.manufacturer_code:04X}, image type 0x{args.image_type:04X}, "
        f"{len(data)} bytes (image {len(image)} bytes)"
    )


if __name__ == "__main__":
    main()
