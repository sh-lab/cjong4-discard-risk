"""Model v2 codec and independent NumPy INT64 inference reference."""
import struct

import numpy as np

from .data import validate_rgb

FIELDS = (
    ("pixel_weights", "i1", (3, 4, 3)),
    ("pixel_bias", "<i4", (3, 4)),
    ("pixel_shift", "u1", (3,)),
    ("hidden_weights", "i1", (8, 544)),
    ("hidden_bias", "<i4", (8,)),
    ("hidden_shift", "u1", (1,)),
    ("output_weights", "i1", (34, 8)),
    ("output_bias", "<i4", (34,)),
    ("output_shift", "u1", (1,)),
)
BANK_RANGES = ((0, 108), (108, 124), (124, 136))
BIAS_LIMIT = 16777216


def validate(parameters):
    if set(parameters) != {name for name, _, _ in FIELDS}:
        raise ValueError("model fields mismatch")
    for name, _, shape in FIELDS:
        value = np.asarray(parameters[name])
        if value.shape != shape or value.dtype.kind not in "iu":
            raise ValueError(f"invalid integer array: {name}")
        low, high = ((-128, 127) if name.endswith("weights") else
                     ((-BIAS_LIMIT, BIAS_LIMIT) if name.endswith("bias") else (0, 31)))
        if np.any(value < low) or np.any(value > high):
            raise ValueError(f"{name} out of range")
    return parameters


def encode(parameters):
    validate(parameters)
    result = struct.pack("<8sHHI", b"CJ4DRI8\0", 2, 3, 4897)
    result += b"".join(np.asarray(parameters[name], dtype=dtype).tobytes()
                       for name, dtype, _ in FIELDS)
    assert len(result) == 4897
    return result


def decode(raw):
    if len(raw) != 4897 or struct.unpack_from("<8sHHI", raw) != (b"CJ4DRI8\0", 2, 3, 4897):
        raise ValueError("invalid model v2 / input v3 header or size")
    parameters, offset = {}, 16
    for name, dtype, shape in FIELDS:
        count = int(np.prod(shape))
        parameters[name] = np.frombuffer(raw, dtype=dtype, count=count, offset=offset).reshape(shape)
        offset += count * np.dtype(dtype).itemsize
    return validate(parameters)


def activate(total, shift, cap):
    return np.minimum(np.maximum(total, 0) >> int(shift), cap)


def predict(parameters, rgb):
    validate(parameters)
    validate_rgb(rgb)
    p = {name: value.astype(np.int64) for name, value in parameters.items()}
    pixels = rgb.astype(np.int64).reshape(-1, 136, 3)
    encoded = []
    for bank, (start, stop) in enumerate(BANK_RANGES):
        total = pixels[:, start:stop] @ p["pixel_weights"][bank].T + p["pixel_bias"][bank]
        encoded.append(activate(total, p["pixel_shift"][bank], 127))
    flat = np.concatenate(encoded, axis=1).reshape(-1, 544)
    hidden = activate(flat @ p["hidden_weights"].T + p["hidden_bias"], p["hidden_shift"][0], 127)
    return activate(hidden @ p["output_weights"].T + p["output_bias"],
                    p["output_shift"][0], 255).astype(np.uint8)


def nonzero_auc(prediction, positive):
    """Rank AUC with exact tie handling, without a sklearn dependency."""
    n_positive = int(positive.sum())
    n_negative = len(positive) - n_positive
    if not n_positive or not n_negative:
        return None
    order = np.argsort(prediction, kind="stable")
    sorted_values = prediction[order]
    starts = np.r_[0, np.flatnonzero(np.diff(sorted_values.astype(np.float64))) + 1]
    stops = np.r_[starts[1:], len(order)]
    ranks = np.repeat((starts + 1 + stops) / 2, stops - starts)
    rank_sum = ranks[positive[order]].sum()
    return float((rank_sum - n_positive * (n_positive + 1) / 2) / (n_positive * n_negative))


def metrics(prediction, target, mask):
    selected = mask.astype(bool)
    count = int(selected.sum())
    if not count:
        raise ValueError("no specified teacher values")
    error = prediction.astype(np.float64) - target
    values = error[selected]
    safe = selected & (target == 0)
    dangerous = selected & (target != 0)
    mse = float(np.mean((values / 255) ** 2))
    baseline = float(np.mean((target[selected].astype(np.float64) / 255) ** 2))
    safe_mse = float(np.mean((error[safe] / 255) ** 2)) if safe.any() else None
    danger_mse = float(np.mean((error[dangerous] / 255) ** 2)) if dangerous.any() else None
    return {
        "labels": count,
        "mse_normalized": mse,
        "zero_baseline_mse": baseline,
        "baseline_improvement": 1 - mse / baseline if baseline else None,
        "mae_gray": float(np.abs(values).mean()),
        "mean_underestimate_gray": float(np.maximum(-values, 0).mean()),
        "safe_labels": int(safe.sum()),
        "safe_mean_prediction": float(prediction[safe].mean()) if safe.any() else None,
        "safe_mse": safe_mse,
        "danger_labels": int(dangerous.sum()),
        "danger_mean_prediction": float(prediction[dangerous].mean()) if dangerous.any() else None,
        "danger_mse": danger_mse,
        "danger_zero_predictions": int((prediction[dangerous] == 0).sum()),
        "all_zero_predictions": bool((prediction[selected] == 0).all()),
        "balanced_mse": (safe_mse + danger_mse) / 2 if safe.any() and dangerous.any() else None,
        "nonzero_auc": nonzero_auc(prediction[selected], target[selected] != 0),
        "per_tile": [
            {"labels": int(selected[:, t].sum()),
             "mae_gray": float(np.abs(error[selected[:, t], t]).mean())
             if selected[:, t].any() else None} for t in range(34)
        ],
    }
