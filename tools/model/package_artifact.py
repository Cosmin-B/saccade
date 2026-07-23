#!/usr/bin/env python3
"""Build and sign a Saccade runtime model artifact using only Python and OpenSSL."""

import argparse
import hashlib
import json
import pathlib
import stat
import struct
import subprocess
import tempfile


ARTIFACT_HEADER_BYTES = 96
SIGNATURE_BYTES = 64
TARGET_RECORD_BYTES = 80
TARGET_PACKET_HEADER_BYTES = 104
PRECISION_FP32 = 1 << 0
PRECISION_FP16 = 1 << 1
PRECISION_INT8 = 1 << 2
COREML_COMPATIBILITY = 1 << 0
DIRECTML_COMPATIBILITY = 1 << 1
HAS_SIGNATURE = 1 << 0
RELATIVE_LOCATOR = 1 << 1
MAXIMUM_BUNDLE_FILES = 4096
MAXIMUM_BUNDLE_RELATIVE_BYTES = 1024
MAXIMUM_BUNDLE_BYTES = 4 * 1024 * 1024 * 1024


def put_u32(buffer, offset, value):
    struct.pack_into("<I", buffer, offset, value)


def put_u64(buffer, offset, value):
    struct.pack_into("<Q", buffer, offset, value)


def der_length(data, offset):
    first = data[offset]
    offset += 1
    if first < 0x80:
        return first, offset
    count = first & 0x7F
    if count == 0 or count > 4 or offset + count > len(data):
        raise ValueError("invalid DER length")
    return int.from_bytes(data[offset:offset + count], "big"), offset + count


def der_value(data, offset, expected_tag):
    if offset >= len(data) or data[offset] != expected_tag:
        raise ValueError("unexpected DER tag")
    size, start = der_length(data, offset + 1)
    end = start + size
    if end > len(data):
        raise ValueError("truncated DER value")
    return data[start:end], end


def public_key_xy(private_key):
    der = subprocess.check_output([
        "openssl", "pkey", "-in", str(private_key), "-pubout", "-outform", "DER"
    ])
    sequence, end = der_value(der, 0, 0x30)
    if end != len(der):
        raise ValueError("invalid public-key DER")
    _, position = der_value(sequence, 0, 0x30)
    bits, position = der_value(sequence, position, 0x03)
    if position != len(sequence) or len(bits) != 66 or bits[:2] != b"\x00\x04":
        raise ValueError("private key is not a P-256 key")
    return bits[2:]


def raw_signature(der):
    sequence, end = der_value(der, 0, 0x30)
    if end != len(der):
        raise ValueError("invalid signature DER")
    r, position = der_value(sequence, 0, 0x02)
    s, position = der_value(sequence, position, 0x02)
    if position != len(sequence):
        raise ValueError("invalid signature sequence")
    r = r.lstrip(b"\x00")
    s = s.lstrip(b"\x00")
    if len(r) > 32 or len(s) > 32:
        raise ValueError("signature is not P-256")
    return r.rjust(32, b"\x00") + s.rjust(32, b"\x00")


def sign(message, private_key):
    with tempfile.NamedTemporaryFile() as source:
        source.write(message)
        source.flush()
        der = subprocess.check_output([
            "openssl", "dgst", "-sha256", "-sign", str(private_key), source.name
        ])
    signature = raw_signature(der)
    if len(signature) != SIGNATURE_BYTES:
        raise ValueError("invalid raw signature size")
    return signature


def stable_id(payload, width, height, precision):
    digest = hashlib.sha256()
    digest.update(struct.pack("<III", width, height, precision))
    digest.update(payload)
    value = int.from_bytes(digest.digest()[:8], "little")
    return value or 1


def directory_digest(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Core ML model bundle must be a directory")
    digest = hashlib.sha256()
    files = []
    total_bytes = 0
    for path in root.rglob("*"):
        status = path.lstat()
        if stat.S_ISLNK(status.st_mode) or not (stat.S_ISDIR(status.st_mode) or stat.S_ISREG(status.st_mode)):
            raise ValueError("Core ML model bundle contains a symlink or special file")
        if stat.S_ISDIR(status.st_mode):
            continue
        relative = path.relative_to(root).as_posix().encode("utf-8")
        if not relative or len(relative) > MAXIMUM_BUNDLE_RELATIVE_BYTES:
            raise ValueError("Core ML model bundle path exceeds the v1 limit")
        if len(files) == MAXIMUM_BUNDLE_FILES:
            raise ValueError("Core ML model bundle file count exceeds the v1 limit")
        if status.st_size > MAXIMUM_BUNDLE_BYTES - total_bytes:
            raise ValueError("Core ML model bundle exceeds the v1 byte limit")
        total_bytes += status.st_size
        files.append((relative, path, status.st_size))
    if not files:
        raise ValueError("Core ML model bundle is empty")
    files.sort(key=lambda item: item[0])
    for relative, path, size in files:
        digest.update(struct.pack("<I", len(relative)))
        digest.update(relative)
        digest.update(struct.pack("<Q", size))
        with path.open("rb") as source:
            while data := source.read(16 * 1024):
                digest.update(data)
    return digest.digest()


def schema_shape(value):
    if isinstance(value, str):
        value = json.loads(value)
    if not isinstance(value, list) or any(not isinstance(dimension, int) for dimension in value):
        raise ValueError("invalid Core ML feature shape")
    return value


def validate_coreml_metadata(args):
    if args.locator != args.model_bundle.name:
        raise ValueError("Core ML locator must match the compiled bundle name")
    metadata_path = args.model_bundle / "metadata.json"
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("Core ML model bundle has no readable metadata.json") from error
    models = metadata if isinstance(metadata, list) else [metadata]
    for model in models:
        if not isinstance(model, dict):
            continue
        inputs = {feature.get("name"): feature for feature in model.get("inputSchema", [])
                  if isinstance(feature, dict)}
        outputs = {feature.get("name"): feature for feature in model.get("outputSchema", [])
                   if isinstance(feature, dict)}
        image = inputs.get(args.input_name)
        rows = outputs.get(args.rows_name)
        count = outputs.get(args.count_name)
        if image is None or rows is None or count is None:
            continue
        if (image.get("type") != "Image" or int(image.get("width", 0)) != args.width or
                int(image.get("height", 0)) != args.height):
            raise ValueError("Core ML image feature does not match the signed contract")
        if (rows.get("type") != "MultiArray" or rows.get("dataType") != "Float32" or
                schema_shape(rows.get("shape")) != [args.candidates, 6]):
            raise ValueError("Core ML target-row feature does not match the signed contract")
        if (count.get("type") != "MultiArray" or count.get("dataType") != "Float32" or
                schema_shape(count.get("shape")) != [1]):
            raise ValueError("Core ML target-count feature does not match the signed contract")
        return
    raise ValueError("Core ML feature names do not match the signed contract")


def artifact(payload, artifact_kind, precision, width, height, maximum_targets,
             compatibility, flags, private_key):
    payload_offset = ARTIFACT_HEADER_BYTES
    signature_offset = payload_offset + len(payload)
    total_size = signature_offset + SIGNATURE_BYTES
    if total_size > 0xFFFFFFFF:
        raise ValueError("artifact exceeds the v1 size limit")
    header = bytearray(ARTIFACT_HEADER_BYTES)
    header[:4] = b"SCMD"
    put_u32(header, 4, 1)
    put_u32(header, 8, ARTIFACT_HEADER_BYTES)
    put_u32(header, 12, total_size)
    put_u64(header, 16, stable_id(payload, width, height, precision))
    put_u32(header, 24, 1)
    put_u32(header, 28, artifact_kind)
    put_u32(header, 32, precision)
    put_u32(header, 36, width)
    put_u32(header, 40, height)
    put_u32(header, 44, 3)
    put_u32(header, 48, maximum_targets)
    put_u32(header, 52, TARGET_PACKET_HEADER_BYTES +
            maximum_targets * TARGET_RECORD_BYTES)
    put_u64(header, 56, payload_offset)
    put_u64(header, 64, len(payload))
    put_u64(header, 72, signature_offset)
    put_u32(header, 80, SIGNATURE_BYTES)
    put_u32(header, 84, HAS_SIGNATURE | flags)
    put_u64(header, 88, compatibility)
    message = bytes(header) + payload
    return message + sign(message, private_key)


def coreml_payload(args):
    validate_coreml_metadata(args)
    values = [args.locator, args.input_name, args.rows_name, args.count_name]
    encoded = [value.encode("ascii") for value in values]
    header = bytearray(112)
    header[:4] = b"SCMC"
    for offset, value in ((4, 4), (8, 1), (12, 1),
                          (16, args.candidates), (20, args.targets),
                          (24, args.confidence_q16), (28, args.iou_q16),
                          (80, args.band_confidence_q16),
                          (84, args.band_min_short_side_q3),
                          (88, args.band_max_short_side_q3)):
        put_u32(header, offset, value)
    for offset, value in zip((32, 36, 40, 44), encoded):
        put_u32(header, offset, len(value))
    header[48:80] = directory_digest(args.model_bundle)
    struct.pack_into("<3f", header, 96, *args.letterbox)
    return bytes(header) + b"".join(encoded)


def directml_payload(args):
    graph = args.onnx.read_bytes()
    names = [args.input_name.encode("ascii"), args.candidates_name.encode("ascii")]
    header = bytearray(96)
    header[:4] = b"SCDM"
    input_kind = 1 if args.precision == "fp16" else 2
    for offset, value in ((4, 2), (8, input_kind), (12, 1),
                          (16, args.candidates), (20, args.targets),
                          (24, args.confidence_q16), (28, args.iou_q16),
                          (32, len(names[0])), (36, len(names[1])),
                          (80, args.band_confidence_q16),
                          (84, args.band_min_short_side_q3),
                          (88, args.band_max_short_side_q3)):
        put_u32(header, offset, value)
    struct.pack_into("<9f", header, 40, *args.scale, *args.bias, *args.letterbox)
    return bytes(header) + b"".join(names) + graph


def bounded_q16(value):
    parsed = int(value, 0)
    if parsed < 0 or parsed > 0xFFFF:
        raise argparse.ArgumentTypeError("value must fit uint16")
    return parsed


def positive(value):
    parsed = int(value, 0)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def add_common(parser):
    parser.add_argument("--key", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--width", type=positive, required=True)
    parser.add_argument("--height", type=positive, required=True)
    parser.add_argument("--candidates", type=positive, required=True)
    parser.add_argument("--targets", type=positive, required=True)
    parser.add_argument("--confidence-q16", type=bounded_q16, default=16384)
    parser.add_argument("--band-confidence-q16", type=bounded_q16, default=0)
    parser.add_argument("--band-min-short-side-q3", type=bounded_q16, default=0)
    parser.add_argument("--band-max-short-side-q3", type=bounded_q16, default=0)
    parser.add_argument("--iou-q16", type=bounded_q16, default=32768)


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    key = commands.add_parser("public-key")
    key.add_argument("--key", type=pathlib.Path, required=True)

    coreml = commands.add_parser("coreml")
    add_common(coreml)
    coreml.add_argument("--locator", required=True)
    coreml.add_argument("--model-bundle", type=pathlib.Path, required=True)
    coreml.add_argument("--precision", choices=("fp16", "fp32"), default="fp16")
    coreml.add_argument("--input-name", required=True)
    coreml.add_argument("--rows-name", required=True)
    coreml.add_argument("--count-name", required=True)
    coreml.add_argument("--letterbox", type=float, nargs=3,
                        default=(0.0, 0.0, 0.0))

    directml = commands.add_parser("directml")
    add_common(directml)
    directml.add_argument("--onnx", type=pathlib.Path, required=True)
    directml.add_argument("--precision", choices=("fp16", "int8"), required=True)
    directml.add_argument("--input-name", required=True)
    directml.add_argument("--candidates-name", required=True)
    directml.add_argument("--scale", type=float, nargs=3, default=(1.0, 1.0, 1.0))
    directml.add_argument("--bias", type=float, nargs=3, default=(0.0, 0.0, 0.0))
    directml.add_argument("--letterbox", type=float, nargs=3,
                          default=(0.0, 0.0, 0.0))
    args = parser.parse_args()

    try:
        if args.command == "public-key":
            print(public_key_xy(args.key).hex())
            return
        if args.targets > args.candidates:
            parser.error("--targets cannot exceed --candidates")
        band_disabled = (args.band_confidence_q16 == 0 and
                         args.band_min_short_side_q3 == 0 and
                         args.band_max_short_side_q3 == 0)
        band_enabled = (0 < args.band_confidence_q16 <= args.confidence_q16 and
                        args.band_min_short_side_q3 < args.band_max_short_side_q3)
        if not band_disabled and not band_enabled:
            parser.error("confidence band must be entirely disabled or define a lower threshold and increasing q3 bounds")
        if args.command == "coreml":
            payload = coreml_payload(args)
            precision = PRECISION_FP16 if args.precision == "fp16" else PRECISION_FP32
            data = artifact(payload, 2, precision, args.width, args.height,
                            args.targets, COREML_COMPATIBILITY, RELATIVE_LOCATOR,
                            args.key)
        else:
            payload = directml_payload(args)
            precision = PRECISION_FP16 if args.precision == "fp16" else PRECISION_INT8
            data = artifact(payload, 3, precision, args.width, args.height,
                            args.targets, DIRECTML_COMPATIBILITY, 0, args.key)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
    except (OSError, UnicodeError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
