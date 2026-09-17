"""Evaluate the exported integer model, optionally compare every result with C."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np

from . import data
from .integer import decode, metrics, predict


def c_compare(executable, model_bytes, rgb, expected):
    executable = str(Path(executable).resolve())
    with tempfile.TemporaryDirectory(prefix="cj4dr-verify-") as directory:
        model_path, input_path = Path(directory) / "model.i8", Path(directory) / "input.rgb"
        model_path.write_bytes(model_bytes)
        for index, row in enumerate(rgb):
            input_path.write_bytes(row.tobytes())
            result = subprocess.run([executable, str(model_path), str(input_path)],
                                    check=True, capture_output=True, text=True)
            actual = [int(value, 16) for value in result.stdout.split()]
            if actual != expected[index].tolist():
                raise ValueError(f"C/Python output mismatch at sample {index}")
    return len(rgb)


def evaluate(raw, dataset, batch_size=1024):
    if batch_size <= 0:
        raise ValueError("batch size must be positive")
    parameters = decode(raw)
    result = np.concatenate([predict(parameters, dataset["rgb"][start:start + batch_size])
                             for start in range(0, len(dataset["rgb"]), batch_size)])
    return result, metrics(result, dataset["target"], dataset["mask"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--exclude", type=Path, action="append", default=[],
                        help="reject overlap with these datasets, e.g. train/validation")
    parser.add_argument("--c-evaluator", type=Path)
    args = parser.parse_args()
    dataset = data.load(args.dataset)
    for excluded in args.exclude:
        data.assert_disjoint(dataset, data.load(excluded))
    raw = args.model.read_bytes()
    result, report = evaluate(raw, dataset)
    if args.c_evaluator:
        report["c_verified_samples"] = c_compare(args.c_evaluator, raw, dataset["rgb"], result)
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
