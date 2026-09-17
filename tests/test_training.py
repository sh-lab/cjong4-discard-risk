"""Synthetic arithmetic/training tests, not mahjong risk-quality evaluation.

Run from repository root: python -m unittest discover -s tests -p test_training.py
Set CJ4DR_EVALUATOR to include native parity and input-validation tests.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np
import torch

from training import data
from training.evaluate import c_compare
from training.integer import BIAS_LIMIT, FIELDS, decode, encode, metrics, predict
from training.model import RiskModel, load_checkpoint, masked_loss

ROOT = Path(__file__).resolve().parents[1]
C_EVALUATOR = os.environ.get("CJ4DR_EVALUATOR")
torch.set_num_threads(1)


def fixture(n=18):
    rng = np.random.default_rng(74)
    rgb = np.full((n, 408), 255, dtype=np.uint8)
    # Valid per-byte encoding, intentionally not claimed to be legal game states.
    rgb[:, 0::3] = rng.integers(0, 30, (n, 136), dtype=np.uint8)
    rgb[:, 2::3] = rng.integers(0, 86, (n, 136), dtype=np.uint8)
    mask = np.zeros((n, 34), dtype=np.uint8)
    mask[:, [0, 4, 33]] = 1
    target = np.full((n, 34), 190, dtype=np.uint8)
    return dict(version=np.array(1, dtype=np.uint16), input_schema=np.array(3, dtype=np.uint16),
                rgb=rgb, target=target, mask=mask,
                group=np.array([f"base-{i // 2}" for i in range(n)]),
                metadata=np.array([json.dumps({"synthetic": True})] * n))


class DataTests(unittest.TestCase):
    def test_pack_roundtrip_and_validation(self):
        source = fixture(2)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "one.rgb").write_bytes(source["rgb"][0].tobytes())
            samples = [{"rgb_file": "one.rgb", "target": source["target"][0].tolist(),
                        "mask": source["mask"][0].tolist(), "group": "game-1",
                        "metadata": {"source": "individual", "日本語": True}},
                       {"rgb_hex": source["rgb"][1].tobytes().hex(),
                        "target": source["target"][1].tolist(),
                        "mask": [0] * 34, "group": "game-2"}]
            path = directory / "source.jsonl"
            path.write_text("\n".join(json.dumps(row) for row in samples))
            packed = data.pack(path)
            np.testing.assert_array_equal(packed["rgb"], source["rgb"])
            data.save(directory / "dataset.npz", packed)
            loaded = data.load(directory / "dataset.npz")
            for key in data.KEYS:
                np.testing.assert_array_equal(loaded[key], packed[key])
            with self.assertRaises(FileExistsError):
                data.save(directory / "dataset.npz", packed)
            for value in (256, -1, 0.5, True):
                samples[0]["target"][0] = value
                path.write_text(json.dumps(samples[0]))
                with self.assertRaises(ValueError):
                    data.pack(path)
        for key, bad in (("mask", np.full((2, 34), 2, dtype=np.uint8)),
                         ("target", np.zeros((2, 34), dtype=np.float32)),
                         ("input_schema", np.array(2, dtype=np.uint16)),
                         ("rgb", np.full((2, 409), 255, dtype=np.uint8))):
            with self.assertRaises(ValueError):
                data.validate(dict(source, **{key: bad}))
        for channel, value in ((0, 31), (1, 32), (1, 135), (2, 127)):
            rgb = source["rgb"].copy()
            rgb[0, channel] = value
            with self.assertRaises(ValueError):
                data.validate_rgb(rgb)

    def test_group_split_and_leakage(self):
        source = fixture()
        parts = data.split(source, seed=4)
        again = data.split(source, seed=4)
        self.assertEqual(sum(len(part["rgb"]) for part in parts), len(source["rgb"]))
        for part, repeated in zip(parts, again):
            np.testing.assert_array_equal(part["rgb"], repeated["rgb"])
        for i in range(3):
            for j in range(i):
                data.assert_disjoint(parts[i], parts[j])
        with self.assertRaises(ValueError):
            data.assert_disjoint(parts[0], parts[0])
        copied = {**parts[0], "group": np.array(["renamed"] * len(parts[0]["rgb"]))}
        with self.assertRaises(ValueError):
            data.assert_disjoint(parts[0], copied)
        with self.assertRaises(ValueError):
            data.split(fixture(2))


class ModelTests(unittest.TestCase):
    def test_masked_loss_gradients_and_learning(self):
        prediction = torch.tensor([[0., 100., 200.]], requires_grad=True)
        mask = torch.tensor([[1., 0., 1.]])
        target = torch.tensor([[255., 0., 0.]])
        loss = masked_loss(prediction, target, mask)
        altered = target.clone()
        altered[0, 1] = 255
        self.assertEqual(loss.item(), masked_loss(prediction, altered, mask).item())
        loss.backward()
        self.assertEqual(prediction.grad[0, 1].item(), 0)
        self.assertNotEqual(prediction.grad[0, 0].item(), 0)
        with self.assertRaises(ValueError):
            masked_loss(prediction, target, torch.zeros_like(mask))
        torch.manual_seed(1)
        model = RiskModel()
        samples = fixture(4)
        x = torch.from_numpy(samples["rgb"])
        y, m = (torch.from_numpy(samples[key]).double() for key in ("target", "mask"))
        before = masked_loss(model(x), y, m).item()
        optimizer = torch.optim.Adam(model.parameters(), lr=0.03)
        for _ in range(40):
            optimizer.zero_grad()
            masked_loss(model(x), y, m).backward()
            self.assertTrue(all(p.grad is not None and torch.isfinite(p.grad).all()
                                for p in model.parameters()))
            optimizer.step()
            model.project()
        self.assertLess(masked_loss(model(x), y, m).item(), before)

    def test_quantized_forward_codec_and_native_parity(self):
        rng = np.random.default_rng(173)
        rgb = fixture(4)["rgb"]
        for shift in range(32):
            shifts = (shift, (shift + 3) % 32, (shift + 7) % 32, shift, (shift + 1) % 32)
            model = RiskModel(shifts)
            with torch.no_grad():
                for name, parameter in model.named_parameters():
                    limit = 127 if name.endswith("weights") else BIAS_LIMIT
                    values = rng.integers(-limit, limit + 1, tuple(parameter.shape))
                    values.flat[0] = -128 if name.endswith("weights") else -BIAS_LIMIT
                    values.flat[-1] = limit
                    parameter.copy_(torch.from_numpy(values) / model.scale(name))
            parameters = model.integers()
            raw = encode(parameters)
            self.assertEqual(len(raw), 4897)
            self.assertEqual(struct.unpack_from("<i", raw, 4760)[0], -BIAS_LIMIT)
            reloaded = decode(raw)
            for name, _, _ in FIELDS:
                np.testing.assert_array_equal(parameters[name], reloaded[name])
            expected = predict(reloaded, rgb)
            np.testing.assert_array_equal(model(torch.from_numpy(rgb)).detach().numpy(), expected)
            if C_EVALUATOR:
                self.assertEqual(c_compare(C_EVALUATOR, raw, rgb, expected), len(rgb))
        for raw in (raw[:-1], raw + b"x", b"wrong!!!" + raw[8:]):
            with self.assertRaises(ValueError):
                decode(raw)
        bad = bytearray(encode(parameters))
        bad[100] = 32
        with self.assertRaises(ValueError):
            decode(bad)

    def test_output_order_and_metrics(self):
        model = RiskModel((0, 0, 0, 0, 0))
        with torch.no_grad():
            for parameter in model.parameters():
                parameter.zero_()
            model.output_bias.copy_(torch.arange(34) * 9 - 10)
        rgb = fixture(1)["rgb"]
        expected = np.clip(np.arange(34) * 9 - 10, 0, 255).astype(np.uint8)[None, :]
        np.testing.assert_array_equal(predict(model.integers(), rgb), expected)
        report = metrics(expected, expected, np.ones((1, 34), dtype=np.uint8))
        self.assertEqual(report["labels"], 34)
        self.assertEqual(report["mae_gray"], 0)

    @unittest.skipUnless(C_EVALUATOR, "set CJ4DR_EVALUATOR for exhaustive C input parity")
    def test_python_input_validation_matches_c(self):
        with tempfile.TemporaryDirectory() as directory:
            model_file, input_file = Path(directory) / "model.i8", Path(directory) / "input.rgb"
            model_file.write_bytes(encode(RiskModel().integers()))
            for channel in range(3):
                for value in range(256):
                    rgb = np.full((1, 408), 255, dtype=np.uint8)
                    rgb[0, channel] = value
                    try:
                        data.validate_rgb(rgb)
                        valid = True
                    except ValueError:
                        valid = False
                    input_file.write_bytes(rgb.tobytes())
                    result = subprocess.run([C_EVALUATOR, str(model_file), str(input_file)],
                                            capture_output=True)
                    self.assertEqual(result.returncode == 0, valid, (channel, value))


class PipelineTests(unittest.TestCase):
    def test_train_resume_export_and_evaluate(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            parts = data.split(fixture())
            parts[0]["mask"][0] = 0  # retained in the file, excluded from training
            paths = [directory / f"{name}.npz" for name in ("train", "validation", "test")]
            for path, part in zip(paths, parts):
                data.save(path, part)

            def run(module, *args, success=True):
                env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", OMP_NUM_THREADS="1",
                           MKL_NUM_THREADS="1")
                result = subprocess.run([sys.executable, "-m", f"training.{module}",
                                         *map(str, args)], cwd=ROOT, env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
                return result

            options = ("--train", paths[0], "--validation", paths[1], "--batch-size", 4)
            first = run("train", *options, "--output", directory / "run", "--epochs", 2)
            self.assertIn("unlabeled skipped=1", first.stdout)
            run("train", *options, "--output", directory / "run", "--epochs", 4,
                "--resume", directory / "run/last.pt")
            run("train", *options, "--output", directory / "full", "--epochs", 4)
            resumed, saved = load_checkpoint(directory / "run/last.pt")
            full, _ = load_checkpoint(directory / "full/last.pt")
            self.assertEqual(saved["epoch"], 4)
            for name, value in resumed.state_dict().items():
                self.assertTrue(torch.equal(value, full.state_dict()[name]), name)
            run("train", *options, "--output", directory / "run", "--epochs", 5,
                "--resume", directory / "run/last.pt", "--lr", 0.01, success=False)
            native = ("--c-evaluator", C_EVALUATOR) if C_EVALUATOR else ()
            run("export", directory / "run/best.pt", directory / "risk.i8",
                "--validation", paths[1], *native)
            self.assertEqual((directory / "risk.i8").stat().st_size, 4897)
            run("export", directory / "run/best.pt", directory / "wrong.i8",
                "--validation", paths[2], success=False)
            result = run("evaluate", directory / "risk.i8", paths[2],
                         "--exclude", paths[0], "--exclude", paths[1], *native)
            self.assertGreater(json.loads(result.stdout)["labels"], 0)
            run("train", "--train", paths[0], "--validation", paths[0],
                "--output", directory / "leak", "--epochs", 1, success=False)
            unlabeled = dict(parts[0], mask=np.zeros_like(parts[0]["mask"]))
            data.save(directory / "unlabeled.npz", unlabeled)
            run("train", "--train", directory / "unlabeled.npz", "--validation", paths[1],
                "--output", directory / "empty", "--epochs", 1, success=False)


if __name__ == "__main__":
    unittest.main()
