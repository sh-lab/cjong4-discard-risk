"""Export a checkpoint as the 4,897-byte C model after integer parity checks."""
import argparse
import json
from pathlib import Path

import numpy as np

from . import data
from .evaluate import c_compare, evaluate
from .integer import encode
from .model import load_checkpoint
from .train import digest, predictions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--validation", type=Path, required=True)
    parser.add_argument("--c-evaluator", type=Path)
    args = parser.parse_args()
    model, saved = load_checkpoint(args.checkpoint)
    if digest(args.validation) != saved["config"]["validation_sha256"]:
        parser.error("use the validation dataset recorded in the checkpoint")
    dataset = data.load(args.validation)
    raw = encode(model.integers())
    prediction, report = evaluate(raw, dataset)
    trained = predictions(model, dataset["rgb"], 1024, "cpu")
    if not np.array_equal(trained, prediction):
        raise ValueError("quantized training forward and integer inference disagree")
    report.update(checkpoint_epoch=saved["epoch"], versions=saved["versions"],
                  config=saved["config"], model_bytes=len(raw),
                  python_verified_samples=len(prediction), c_verified_samples=0)
    if args.c_evaluator:
        report["c_verified_samples"] = c_compare(args.c_evaluator, raw, dataset["rgb"], prediction)
    report_path = args.output.with_name(args.output.name + ".json")
    if args.output.exists() or report_path.exists():
        parser.error("output or report already exists")
    with args.output.open("xb") as output:
        output.write(raw)
    with report_path.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2, allow_nan=False)
        output.write("\n")
    print(f"exported {len(raw)} bytes; Python parity {len(prediction)} samples; "
          f"C parity {report['c_verified_samples']} samples")


if __name__ == "__main__":
    main()
