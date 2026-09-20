"""Quantization-aware training of the exact C inference topology.

Double precision preserves every legal integer accumulator exactly on CPU/CUDA.
Only backward uses the straight-through estimator (STE).
"""
import numpy as np
import torch
from torch import nn
from torch.nn import functional as F

from .integer import BANK_RANGES, BIAS_LIMIT, validate


def round_ste(value):
    return value + (value.round() - value).detach()


class QuantizedActivation(torch.autograd.Function):
    """Exact integer forward, leaky surrogate derivative outside the legal range.

    Saturated outputs must be able to recover from a wrong zero/maximum prediction.
    This derivative is training-only; the deployed C activation is unchanged.
    """
    @staticmethod
    def forward(ctx, value, cap):
        ctx.save_for_backward(value)
        ctx.cap = cap
        return value.clamp(0, cap).floor()

    @staticmethod
    def backward(ctx, gradient):
        (value,) = ctx.saved_tensors
        slope = torch.where((value >= 0) & (value <= ctx.cap), 1.0, 0.01)
        return gradient * slope, None


def activate(value, cap):
    return QuantizedActivation.apply(value, cap)


class RiskModel(nn.Module):
    def __init__(self, shifts=(3, 3, 3, 6, 4)):
        super().__init__()
        if len(shifts) != 5 or any(type(x) is not int or not 0 <= x <= 31 for x in shifts):
            raise ValueError("five integer shifts in 0..31 required")
        self.shifts = tuple(shifts)
        for name, shape in (("pixel_weights", (3, 4, 3)), ("pixel_bias", (3, 4)),
                            ("hidden_weights", (8, 544)), ("hidden_bias", (8,)),
                            ("output_weights", (34, 8)), ("output_bias", (34,))):
            self.register_parameter(name, nn.Parameter(torch.empty(shape, dtype=torch.float64)))
        with torch.no_grad():
            self.pixel_weights.normal_(0, 0.15)
            self.pixel_bias.fill_(32)
            self.hidden_weights.normal_(0, 0.03)
            self.hidden_bias.fill_(48)
            self.output_weights.normal_(0, 0.05)
            self.output_bias.fill_(128)
        self.project()

    def scale(self, name):
        if name.startswith("pixel"):
            shape = (3, 1, 1) if name.endswith("weights") else (3, 1)
            return torch.tensor([2.0 ** s for s in self.shifts[:3]],
                                device=self.pixel_weights.device, dtype=torch.float64).reshape(shape)
        return 2.0 ** self.shifts[3 if name.startswith("hidden") else 4]

    def quantized(self, name):
        parameter = getattr(self, name)
        low, high = (-128, 127) if name.endswith("weights") else (-BIAS_LIMIT, BIAS_LIMIT)
        return round_ste((parameter * self.scale(name)).clamp(low, high))

    @torch.no_grad()
    def project(self):
        for name, parameter in self.named_parameters():
            scale = self.scale(name)
            low, high = (-128, 127) if name.endswith("weights") else (-BIAS_LIMIT, BIAS_LIMIT)
            parameter.copy_((parameter * scale).clamp(low, high) / scale)

    def forward(self, rgb):
        if rgb.ndim != 2 or rgb.shape[1] != 408:
            raise ValueError("input must be [N,408]")
        pixels = rgb.to(torch.float64).reshape(-1, 136, 3)
        weights, bias = self.quantized("pixel_weights"), self.quantized("pixel_bias")
        encoded = [activate(F.linear(pixels[:, start:stop], weights[bank], bias[bank]) /
                            (2 ** self.shifts[bank]), 127)
                   for bank, (start, stop) in enumerate(BANK_RANGES)]
        flat = torch.cat(encoded, dim=1).reshape(-1, 544)
        hidden = activate(F.linear(flat, self.quantized("hidden_weights"),
                                   self.quantized("hidden_bias")) / (2 ** self.shifts[3]), 127)
        return activate(F.linear(hidden, self.quantized("output_weights"),
                                 self.quantized("output_bias")) / (2 ** self.shifts[4]), 255)

    @torch.no_grad()
    def integers(self):
        if any(not torch.isfinite(p).all() for p in self.parameters()):
            raise ValueError("non-finite model parameters")
        result = {name: self.quantized(name).cpu().numpy().astype(np.int64)
                  for name, _ in self.named_parameters()}
        result.update(pixel_shift=np.array(self.shifts[:3]),
                      hidden_shift=np.array(self.shifts[3:4]),
                      output_shift=np.array(self.shifts[4:5]))
        return validate(result)


def masked_loss(prediction, target, mask):
    count = mask.sum()
    if count.item() == 0:
        raise ValueError("batch has no teacher values")
    return ((((prediction - target) / 255) ** 2) * mask).sum() / count


def load_checkpoint(path):
    saved = torch.load(path, map_location="cpu", weights_only=True)
    if (saved.get("format_version") not in (1, 2) or
            (saved.get("input_schema"), saved.get("model_version")) != (3, 2)):
        raise ValueError("unsupported training checkpoint")
    model = RiskModel(tuple(saved["shifts"]))
    model.load_state_dict(saved["state_dict"], strict=True)
    model.integers()  # reject non-finite parameters before use
    return model, saved
