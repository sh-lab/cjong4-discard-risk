"""Train masked grayscale targets; save best and resumable last checkpoints."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path

# Required for deterministic CUDA matrix multiplication; set before importing torch.
os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
import numpy as np
import torch

from . import data
from .integer import metrics
from .model import RiskModel, load_checkpoint, masked_loss


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def checkpoint(path, state):
    temporary = path.with_suffix(".tmp")
    torch.save(state, temporary)
    temporary.replace(path)


@torch.no_grad()
def predictions(model, rgb, batch_size, device):
    model.eval()
    chunks = [model(torch.from_numpy(rgb[start:start + batch_size]).to(device)).cpu().numpy()
              for start in range(0, len(rgb), batch_size)]
    result = np.concatenate(chunks)
    if not np.isfinite(result).all():
        raise ValueError("non-finite validation output")
    return result.astype(np.uint8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train", type=Path, required=True)
    parser.add_argument("--validation", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--lr", type=float, default=0.03)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--shifts", type=int, nargs=5, default=[3, 3, 3, 6, 4])
    parser.add_argument("--resume", type=Path)
    args = parser.parse_args()
    if args.epochs <= 0 or args.batch_size <= 0 or not math.isfinite(args.lr) or args.lr <= 0:
        parser.error("epochs, batch-size and lr must be positive")
    if not 0 <= args.seed < 2 ** 63:
        parser.error("seed must be an integer in 0..2^63-1")
    if args.device == "cuda" and not torch.cuda.is_available():
        parser.error("CUDA is unavailable in this Python environment")
    torch.manual_seed(args.seed)
    torch.use_deterministic_algorithms(True)
    training, validation = data.load(args.train), data.load(args.validation)
    data.assert_disjoint(training, validation)
    indices = np.flatnonzero(training["mask"].any(axis=1))
    if not len(indices) or not validation["mask"].any():
        parser.error("train and validation must each have specified teacher values")
    print(f"training samples={len(indices)}, unlabeled skipped={len(training['rgb']) - len(indices)}",
          flush=True)
    config = dict(train_sha256=digest(args.train), validation_sha256=digest(args.validation),
                  batch_size=args.batch_size, lr=args.lr, seed=args.seed, shifts=args.shifts)
    start, best = 0, float("inf")
    saved = None
    if args.resume:
        if args.resume.resolve() != (args.output / "last.pt").resolve():
            parser.error("resume must use last.pt in the same output directory")
        model, saved = load_checkpoint(args.resume)
        if saved["config"] != config:
            parser.error("resume data/config differs; reuse the original options")
        start, best = saved["epoch"], saved["best_mse"]
        if start >= args.epochs:
            parser.error("epochs is the total; it must exceed the saved epoch")
    else:
        model = RiskModel(tuple(args.shifts))
        args.output.mkdir(parents=True, exist_ok=False)
    model.to(args.device)
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)
    if saved:
        optimizer.load_state_dict(saved["optimizer"])
    versions = {"torch": str(torch.__version__), "numpy": str(np.__version__)}
    for epoch in range(start, args.epochs):
        model.train()
        # Epoch-local shuffle permits deterministic resume without serialized RNG objects.
        order = np.random.default_rng(args.seed + epoch).permutation(indices)
        total_loss, labels = 0.0, 0
        for offset in range(0, len(order), args.batch_size):
            chosen = order[offset:offset + args.batch_size]
            rgb = torch.from_numpy(training["rgb"][chosen]).to(args.device)
            target = torch.from_numpy(training["target"][chosen]).to(args.device, torch.float64)
            mask = torch.from_numpy(training["mask"][chosen]).to(args.device, torch.float64)
            optimizer.zero_grad(set_to_none=True)
            loss = masked_loss(model(rgb), target, mask)
            if not torch.isfinite(loss):
                raise ValueError("non-finite loss")
            loss.backward()
            optimizer.step()
            model.project()
            count = int(mask.sum().item())
            total_loss += loss.item() * count
            labels += count
        prediction = predictions(model, validation["rgb"], args.batch_size, args.device)
        report = metrics(prediction, validation["target"], validation["mask"])
        improved = report["mse_normalized"] < best
        best = min(best, report["mse_normalized"])
        state = dict(format_version=1, input_schema=3, model_version=2,
                     shifts=list(model.shifts), state_dict=model.state_dict(),
                     optimizer=optimizer.state_dict(), epoch=epoch + 1, best_mse=best,
                     config=config, versions=versions, validation=report)
        if improved:
            checkpoint(args.output / "best.pt", state)
        checkpoint(args.output / "last.pt", state)
        record = dict(epoch=epoch + 1, train_mse=total_loss / labels, validation=report,
                      best_mse=best)
        with (args.output / "metrics.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps(record, allow_nan=False) + "\n")
        print(f"epoch={epoch + 1} train_mse={total_loss / labels:.6f} "
              f"validation_mse={report['mse_normalized']:.6f} "
              f"validation_mae={report['mae_gray']:.3f}", flush=True)


if __name__ == "__main__":
    main()
