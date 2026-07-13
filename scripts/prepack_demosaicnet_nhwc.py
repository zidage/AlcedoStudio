#!/usr/bin/env python3
"""Create standalone DemosaicNet safetensors with offline NHWC trunk weights."""

import argparse
import json
import struct
from pathlib import Path


def read_safetensors(path: Path):
    blob = path.read_bytes()
    header_size = struct.unpack_from("<Q", blob, 0)[0]
    header_end = 8 + header_size
    return json.loads(blob[8:header_end]), blob[header_end:]


def transform_oihw_to_ckco(raw: bytes, channels: int) -> bytes:
    values = memoryview(raw).cast("f")
    out = [0.0] * len(values)
    for ci in range(channels):
        for kernel in range(9):
            for co in range(channels):
                out[(ci * 9 + kernel) * channels + co] = values[(co * channels + ci) * 9 + kernel]
    return struct.pack(f"<{len(out)}f", *out)


def write_variant(source: Path, destination: Path, depth: int, channels: int) -> None:
    header, payload = read_safetensors(source)
    metadata = dict(header.pop("__metadata__"))
    metadata["channels_last_square_trunk"] = "CKCO"
    metadata["channels_last_source"] = source.name

    entries = []
    for name, entry in header.items():
        start, end = entry["data_offsets"]
        entries.append((name, entry["dtype"], entry["shape"], payload[start:end]))
    for layer in range(1, depth):
        key = f"trunk.{layer}.weight"
        entry = header[key]
        start, end = entry["data_offsets"]
        entries.append((f"trunk.{layer}.nhwc_weight", "F32", [channels, 3, 3, channels],
                        transform_oihw_to_ckco(payload[start:end], channels)))

    new_header = {"__metadata__": metadata}
    offset = 0
    data_parts = []
    for name, dtype, shape, data in entries:
        new_header[name] = {"dtype": dtype, "shape": shape, "data_offsets": [offset, offset + len(data)]}
        offset += len(data)
        data_parts.append(data)
    encoded = json.dumps(new_header, separators=(",", ":"), sort_keys=True).encode("utf-8")
    destination.write_bytes(struct.pack("<Q", len(encoded)) + encoded + b"".join(data_parts))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path, default=Path("alcedo_studio/src/config/models"))
    args = parser.parse_args()
    write_variant(args.model_dir / "bayer.safetensors", args.model_dir / "bayer_nhwc.safetensors", 8, 24)
    write_variant(args.model_dir / "xtrans.safetensors", args.model_dir / "xtrans_nhwc.safetensors", 4, 32)


if __name__ == "__main__":
    main()
