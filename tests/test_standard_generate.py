"""Standard self-play: genuine pre-discard snapshots, outcomes and game isolation."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())


def offset(tile):
    tile_type, copy = divmod(tile, 4)
    if tile_type < 27:
        return (tile_type // 9) * 108 + (copy * 9 + tile_type % 9) * 3
    first, width, start = (27, 4, 324) if tile_type < 31 else (31, 3, 372)
    return start + (copy * width + tile_type - first) * 3


with tempfile.TemporaryDirectory(prefix="cj4dr-standard-") as directory:
    root = Path(directory)
    def run(name, *args, success=True):
        path = root / name
        result = subprocess.run([exe, "--output", str(path), *map(str, args)],
                                capture_output=True, text=True, timeout=300)
        assert (result.returncode == 0) == success, result.stderr
        assert not Path(str(path) + ".partial").exists()
        return path

    path = run("data.jsonl", "--games", 1, "--seed", 82000)
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    assert len(rows) > 4
    assert len({row["group"] for row in rows}) == 1
    assert len({row["metadata"]["round"] for row in rows}) > 1
    assert {row["metadata"]["player"] for row in rows} == {0, 1, 2, 3}
    positive, ordinary = [], []
    for row in rows:
        metadata = row["metadata"]
        rgb = bytes.fromhex(row["rgb_hex"])
        assert len(rgb) == 408
        assert metadata["source"] == "standard-selfplay"
        assert metadata["generator_version"] == 2
        assert metadata["teacher_policy"] == "staged-v1"
        assert len(row["target"]) == len(row["mask"]) == 34
        assert set(row["target"]) <= {0, 16, 32, 64, 96, 128, 160, 192, 255}
        assert set(row["mask"]) <= {0, 1}
        assert not any(x in (32, 64, 96) for x in rgb[1::3])
        tile = metadata["discarded_tile"]
        if tile >= 0:
            # In a post-discard view placement would be 255; it must still be self's hand.
            assert rgb[offset(tile) + 1] == 0
            assert row["mask"][tile // 4] == 1
        for t, label in enumerate(row["mask"]):
            if label:
                assert any(rgb[offset(t * 4 + copy) + 1] == 0 for copy in range(4))
        if metadata["actual_ron"]:
            positive.append(row)
            assert tile >= 0 and row["target"][tile // 4] == 255
            assert row["target"].count(255) == 1
            assert metadata["winner_mask"] != 0
            assert not metadata["winner_mask"] & (1 << metadata["player"])
        else:
            ordinary.append(row)
            assert 255 not in row["target"]
            assert metadata["winner_mask"] == 0
    assert positive and ordinary
    assert any(0 < v < 255 for row in ordinary for v, m in zip(row["target"], row["mask"]) if m)
    assert rows[0]["group"] == "standard-v1/seed-82000/game-0"
    assert {v for v, m in zip(rows[0]["target"], rows[0]["mask"]) if m} == {64}
    # Same game's trace is independent of batch/shard numbering.
    repeat = run("repeat.jsonl", "--games", 1, "--start-game", 0, "--seed", 82000)
    assert repeat.read_bytes() == path.read_bytes()
    before = path.read_bytes()
    run("data.jsonl", "--games", 1, success=False)
    assert path.read_bytes() == before
    for i, arguments in enumerate((("--games", 0), ("--games", "-1"),
                                    ("--max-steps", 1), ("--start-game", str(2**64 - 1)))):
        invalid = run(f"bad-{i}.jsonl", *arguments, success=False)
        assert not invalid.exists()
print(f"Standard: {len(rows)} pre-discard rows, {len(positive)} actual ron; replay/group/mask checks passed")
