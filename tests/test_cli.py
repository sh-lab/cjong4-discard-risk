"""End-to-end CLI contract tests; synthetic weights do not measure risk quality."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="cj4dr-cli-") as tmp:
    root = Path(tmp)
    model = bytearray(4897)
    struct.pack_into("<8sHHI", model, 0, b"CJ4DRI8\0", 2, 3, 4897)
    expected = [min(t * 8, 255) for t in range(34)]
    for t in range(34):
        struct.pack_into("<i", model, 4760 + t * 4, t * 8)
    (root / "model.i8").write_bytes(model)
    data = bytes([255] * 408)
    (root / "input.rgb").write_bytes(data)

    def run(model_name="model.i8", input_name="input.rgb", pgm=False):
        args = [exe] + (["--pgm"] if pgm else [])
        return subprocess.run(args + [str(root / model_name), str(root / input_name)],
                              capture_output=True, text=True, check=False)

    result = run()
    assert result.returncode == 0, result.stderr
    assert result.stdout == " ".join(f"{value:02X}" for value in expected) + "\n"
    result = run(pgm=True)
    assert result.returncode == 0, result.stderr
    tokens = result.stdout.split()
    assert tokens[:4] == ["P2", "34", "1", "255"]
    assert list(map(int, tokens[4:])) == expected

    for body in (data[:-1], data + b"\0", data + bytes(6),
                 data[:-1] + bytes([127])):
        (root / "bad.rgb").write_bytes(body)
        result = run(input_name="bad.rgb")
        assert result.returncode != 0 and not result.stdout
    old_model = bytearray(model)
    old_model[8] = 1
    old_schema = bytearray(model)
    old_schema[10] = 2
    for body in (model[:-1], model + b"\0", bytes(5589), old_model, old_schema):
        (root / "bad.i8").write_bytes(body)
        for pgm in (False, True):
            result = run(model_name="bad.i8", pgm=pgm)
            assert result.returncode != 0 and not result.stdout
    result = run(model_name="missing.i8")
    assert result.returncode != 0 and not result.stdout
    result = subprocess.run([exe], capture_output=True, text=True, check=False)
    assert result.returncode == 2 and not result.stdout
print("CLI: 34 hex scores, 34x1 PGM, and invalid/legacy data rejection passed")
