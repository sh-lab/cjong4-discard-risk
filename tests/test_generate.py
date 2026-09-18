"""Generator CLI contract and reproducibility tests (stdlib only)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())

with tempfile.TemporaryDirectory(prefix="cj4dr-generate-") as directory:
    root = Path(directory)

    def run(name, *args, success=True):
        path = root / name
        result = subprocess.run([exe, "--output", str(path), *map(str, args)],
                                capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stderr
        assert not Path(str(path) + ".partial").exists()
        return path

    def read(path):
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        assert rows
        for row in rows:
            assert len(bytes.fromhex(row["rgb_hex"])) == 408
            assert len(row["target"]) == len(row["mask"]) == 34
            assert set(row["target"]) <= {0, 255}
            assert set(row["mask"]) <= {0, 1} and any(row["mask"])
            assert all(t == 0 for t, m in zip(row["target"], row["mask"]) if not m)
            pixels = bytes.fromhex(row["rgb_hex"])
            assert not any(x in (32, 64, 96) for x in pixels[1::3])
        return rows

    manual = run("manual.jsonl", "--mode", "cases", "--variants", 2, "--seed", 1)
    rows = read(manual)
    assert len(rows) == 204 and len({r["group"] for r in rows}) == 34
    assert sum(sum(r["target"]) // 255 for r in rows) == 68
    for row in rows:
        t = row["metadata"]["target_type"]
        assert sum(row["mask"]) == 1 and row["mask"][t] == 1
        assert row["target"][t] == (255 if row["metadata"]["source"] == "riichi-danger" else 0)
    repeated = run("repeat.jsonl", "--mode", "cases", "--variants", 2, "--seed", 1)
    assert repeated.read_bytes() == manual.read_bytes()
    before = manual.read_bytes()
    run("manual.jsonl", "--mode", "cases", success=False)
    assert manual.read_bytes() == before

    random = run("random.jsonl", "--rounds", 2, "--seed", 11)
    rows = read(random)
    assert len({r["group"] for r in rows}) == 2
    for index in range(2):
        shard = run(f"shard-{index}.jsonl", "--rounds", 1, "--start-round", index, "--seed", 11)
        assert read(shard) == [r for r in rows if r["metadata"]["index"] == index]
    read(run("uniform.jsonl", "--rounds", 1, "--epsilon-percent", 100))
    for index, args in enumerate((("--rounds", "-1"), ("--seed", "+1"),
                                  ("--mode", "invalid"), ("--epsilon-percent", 101),
                                  ("--variants", 0), ("--max-steps", 1))):
        bad = run(f"bad-{index}.jsonl", *args, success=False)
        assert not bad.exists()
    if os.name == "posix":
        interrupted = root / "interrupted.jsonl"
        process = subprocess.Popen([exe, "--rounds", "100000", "--output", str(interrupted)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            assert "completed" in process.stderr.readline()
            process.terminate()
            process.communicate(timeout=15)
            assert process.returncode != 0
            assert not interrupted.exists()
            assert not Path(str(interrupted) + ".partial").exists()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
print("Generator: legal cases, schemas, sharding, reproducibility and failure cleanup passed")
