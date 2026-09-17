"""Lossless RGB datasets and group-isolated splits. No feature extraction."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

KEYS = {"version", "input_schema", "rgb", "target", "mask", "group", "metadata"}


def validate_rgb(rgb):
    if rgb.dtype != np.uint8 or rgb.ndim != 2 or rgb.shape[1] != 408:
        raise ValueError("rgb must be uint8 [N,408]")
    pixels = rgb.reshape(-1, 136, 3)
    discard, placement, history = (pixels[:, :, i] for i in range(3))
    valid = ((discard == 255) | ((discard & 31) <= 30))
    valid &= ((placement == 255) | (placement == 0) |
              (((placement & 128) != 0) & ((placement & 7) <= 4)))
    valid &= ((history == 255) | ((history & 127) <= 85))
    if not valid.all():
        raise ValueError("invalid input v3 bytes (including unmasked opponent hands)")


def validate(data):
    if set(data) != KEYS:
        raise ValueError("dataset keys do not match format v1")
    for key, expected in (("version", 1), ("input_schema", 3)):
        if data[key].shape != () or data[key].dtype != np.uint16 or data[key].item() != expected:
            raise ValueError(f"unsupported {key}")
    validate_rgb(data["rgb"])
    n = len(data["rgb"])
    if n == 0:
        raise ValueError("empty dataset")
    for key in ("target", "mask"):
        if data[key].dtype != np.uint8 or data[key].shape != (n, 34):
            raise ValueError(f"{key} must be uint8 [N,34]")
    if not np.isin(data["mask"], [0, 1]).all():
        raise ValueError("mask must contain only 0 or 1")
    for key in ("group", "metadata"):
        if data[key].dtype.kind != "U" or data[key].shape != (n,):
            raise ValueError(f"{key} must be Unicode [N]")
    if any(not group.strip() for group in data["group"]):
        raise ValueError("group must be a nonempty game/base-case ID")
    for metadata in data["metadata"]:
        if not isinstance(json.loads(metadata), dict):
            raise ValueError("metadata must encode a JSON object")
    return data


def load(path):
    with np.load(path, allow_pickle=False) as archive:
        return validate({key: archive[key] for key in archive.files})


def save(path, data):
    validate(data)
    # Open explicitly: numpy must not silently append another suffix.
    with Path(path).open("xb") as output:
        np.savez_compressed(output, **data)


def subset(data, indices):
    return {key: value if value.ndim == 0 else value[indices] for key, value in data.items()}


def assert_disjoint(left, right):
    if set(left["group"]) & set(right["group"]):
        raise ValueError("game/base-case groups overlap between datasets")
    # Also catch exact images copied with a different group ID.
    if {row.tobytes() for row in left["rgb"]} & {row.tobytes() for row in right["rgb"]}:
        raise ValueError("identical RGB inputs overlap between datasets")


def split(data, validation_fraction=0.1, test_fraction=0.1, seed=1):
    if not (0 < validation_fraction < 1 and 0 < test_fraction < 1 and
            validation_fraction + test_fraction < 1):
        raise ValueError("validation/test fractions must be positive and sum to less than 1")
    groups = sorted(set(data["group"]), key=lambda g: hashlib.sha256(
        f"{seed}:{g}".encode()).digest())
    nv = max(1, round(len(groups) * validation_fraction))
    nt = max(1, round(len(groups) * test_fraction))
    if nv + nt >= len(groups):
        raise ValueError("need more independent groups for three nonempty splits")
    memberships = (groups[nv + nt:], groups[:nv], groups[nv:nv + nt])
    parts = [subset(data, np.isin(data["group"], group)) for group in memberships]
    for i in range(3):
        for j in range(i):
            assert_disjoint(parts[i], parts[j])
    return parts


def pack(source):
    rows = []
    source = Path(source)
    with source.open(encoding="utf-8") as lines:
        for line_number, line in enumerate(lines, 1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
                if not isinstance(item, dict):
                    raise ValueError("sample must be an object")
                if set(item) - {"rgb_file", "rgb_hex", "target", "mask", "group", "metadata"}:
                    raise ValueError("unknown sample field")
                if ("rgb_file" in item) == ("rgb_hex" in item):
                    raise ValueError("specify exactly one of rgb_file/rgb_hex")
                raw = ((source.parent / item["rgb_file"]).read_bytes() if "rgb_file" in item
                       else bytes.fromhex(item["rgb_hex"]))
                if len(raw) != 408:
                    raise ValueError("RGB must be exactly 408 bytes")
                for key, maximum in (("target", 255), ("mask", 1)):
                    values = item[key]
                    if (not isinstance(values, list) or len(values) != 34 or
                            any(type(x) is not int or not 0 <= x <= maximum for x in values)):
                        raise ValueError(f"{key} must contain 34 integers in 0..{maximum}")
                group = item["group"]
                if not isinstance(group, str) or not group.strip():
                    raise ValueError("group must be a nonempty string")
                metadata = item.get("metadata", {})
                if not isinstance(metadata, dict):
                    raise ValueError("metadata must be an object")
                rows.append((np.frombuffer(raw, dtype=np.uint8), item["target"], item["mask"],
                             group, json.dumps(metadata, ensure_ascii=False, allow_nan=False)))
            except (ValueError, KeyError, TypeError, OSError) as error:
                raise ValueError(f"{source}:{line_number}: {error}") from error
    if not rows:
        raise ValueError("no samples")
    return validate(dict(version=np.array(1, dtype=np.uint16),
                         input_schema=np.array(3, dtype=np.uint16),
                         rgb=np.stack([r[0] for r in rows]),
                         target=np.array([r[1] for r in rows], dtype=np.uint8),
                         mask=np.array([r[2] for r in rows], dtype=np.uint8),
                         group=np.array([r[3] for r in rows]),
                         metadata=np.array([r[4] for r in rows])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack_parser = commands.add_parser("pack")
    pack_parser.add_argument("source", type=Path)
    pack_parser.add_argument("output", type=Path)
    split_parser = commands.add_parser("split")
    split_parser.add_argument("source", type=Path)
    split_parser.add_argument("directory", type=Path)
    split_parser.add_argument("--validation-fraction", type=float, default=0.1)
    split_parser.add_argument("--test-fraction", type=float, default=0.1)
    split_parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    if args.command == "pack":
        data = pack(args.source)
        save(args.output, data)
        print(f"saved {len(data['rgb'])} samples, {int(data['mask'].sum())} labels")
    else:
        parts = split(load(args.source), args.validation_fraction, args.test_fraction, args.seed)
        args.directory.mkdir(parents=True, exist_ok=False)
        for name, part in zip(("train", "validation", "test"), parts):
            save(args.directory / f"{name}.npz", part)
            print(f"{name}: {len(part['rgb'])} samples, {len(set(part['group']))} groups")


if __name__ == "__main__":
    main()
