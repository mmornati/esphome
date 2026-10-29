#!/usr/bin/env python3
"""Wrap an ESP-IDF app image into a Zigbee OTA Upgrade file (ZCL spec 11.4).

The file is a 56-byte header followed by one sub-element that holds the app
image, either as is (tag 0x0000, Upgrade Image) or zlib-compressed (tag
0xF100, manufacturer specific), which the zigbee OTA platform inflates on the
device.

With --delta-base, a second file holds a delta instead (tag 0xF101): an
esp_delta_ota patch (detools, heatshrink) against the image the device runs,
behind a 64-byte header with the magic and that image's validation hash, so a
device running another image refuses it. Only the delta needs detools; the
rest is standard library.

With --archive, every image is kept as <archive>/<FILE VERSION>.bin, so a
later build can make a delta against the version a device runs.
"""

import argparse
import hashlib
import io
from pathlib import Path
import shutil
import struct
import zlib

FILE_IDENTIFIER = 0x0BEEF11E
HEADER_VERSION = 0x0100
HEADER_LENGTH = 56
STACK_VERSION_ZIGBEE_PRO = 0x0002
TAG_UPGRADE_IMAGE = 0x0000
TAG_IMAGE_ZLIB = 0xF100
TAG_IMAGE_DELTA = 0xF101
ESP_IMAGE_MAGIC = 0xE9
DELTA_MAGIC = 0xFCCDDE10
DELTA_HEADER_LEN = 64


def validation_hash(image: bytes) -> bytes:
    """The SHA-256 that ESP-IDF appends to an app image, the value
    esp_partition_get_sha256() returns for the partition that holds it (what
    esptool image_info calls the validation hash). A signature block after it
    is not part of the image."""
    if len(image) < 24 or image[0] != ESP_IMAGE_MAGIC or image[23] != 1:
        raise SystemExit("not an ESP app image with an appended SHA-256")
    pos = 24
    for _ in range(image[1]):
        _, length = struct.unpack_from("<II", image, pos)
        pos += 8 + length
    pos += 1  # checksum byte, then padding to a 16-byte boundary
    pos = (pos + 15) & ~15
    digest = image[pos : pos + 32]
    if len(digest) != 32 or hashlib.sha256(image[:pos]).digest() != digest:
        raise SystemExit("app image hash does not match its content")
    return digest


def delta_patch(base: bytes, new: bytes) -> bytes:
    """An esp_delta_ota patch from base to new, checked before it is used."""
    try:
        import detools
    except ImportError as err:
        raise SystemExit("delta images need detools: pip install detools") from err
    patch = io.BytesIO()
    detools.create_patch(
        io.BytesIO(base), io.BytesIO(new), patch, compression="heatshrink"
    )
    body = patch.getvalue()
    check = io.BytesIO()
    detools.apply_patch(io.BytesIO(base), io.BytesIO(body), check)
    if check.getvalue() != new:
        raise SystemExit("the delta does not reproduce the new image")
    header = struct.pack("<I", DELTA_MAGIC) + validation_hash(base)
    return header.ljust(DELTA_HEADER_LEN, b"\0") + body


def build(
    image: bytes,
    manufacturer_code: int,
    image_type: int,
    file_version: int,
    header_string: str,
    compress: bool,
    delta_base: bytes | None = None,
) -> bytes:
    if delta_base is not None:
        tag, payload = TAG_IMAGE_DELTA, delta_patch(delta_base, image)
    elif compress:
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
    parser.add_argument(
        "--archive", type=Path, help="keep the image here as <FILE VERSION>.bin"
    )
    parser.add_argument(
        "--delta-base",
        help="image the device runs: a file, or a file version kept in --archive",
    )
    parser.add_argument(
        "--delta-output", type=Path, help="Zigbee OTA file for the delta"
    )
    args = parser.parse_args()

    image = args.image.read_bytes()
    common = (
        args.manufacturer_code,
        args.image_type,
        args.file_version,
        args.header_string,
    )
    data = build(image, *common, not args.no_compress)
    args.output.write_bytes(data)
    print(
        f"Zigbee OTA file {args.output.name}: version 0x{args.file_version:08X}, "
        f"manufacturer 0x{args.manufacturer_code:04X}, image type 0x{args.image_type:04X}, "
        f"{len(data)} bytes (image {len(image)} bytes)"
    )
    if args.archive is not None:
        args.archive.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.image, args.archive / f"{args.file_version:08X}.bin")

    if args.delta_base and args.delta_output:
        base_path = Path(args.delta_base)
        if not base_path.is_file() and args.archive is not None:
            base_path = args.archive / f"{int(args.delta_base, 0):08X}.bin"
        if not base_path.is_file():
            print(
                f"No image {args.delta_base} to make a delta from: no {args.delta_output.name}"
            )
            args.delta_output.unlink(missing_ok=True)
            return
        delta = build(image, *common, False, base_path.read_bytes())
        args.delta_output.write_bytes(delta)
        print(
            f"Zigbee OTA delta {args.delta_output.name}: from {base_path.name}, "
            f"{len(delta)} bytes ({len(delta) * 100 // len(data)} % of the full file)"
        )


if __name__ == "__main__":
    main()
