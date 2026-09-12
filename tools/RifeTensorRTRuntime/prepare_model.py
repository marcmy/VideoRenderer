#!/usr/bin/env python3
"""Prepare the mixed-precision RIFE 4.6 ONNX consumed by MPCVR.

Input is the upstream AmusementClub/vs-mlrt ``rife_v8.7z`` model archive.
The script extracts the original Practical-RIFE-derived v4.6 ONNX, converts it
with NVIDIA Model Optimizer AutoCast, validates the public tensor contract, and
writes a single self-contained ONNX file.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path

import numpy as np
import onnx
import py7zr
from modelopt.onnx.autocast import convert_to_mixed_precision


CALIBRATION_WIDTH = 1024
CALIBRATION_HEIGHT = 576


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tensor_shape(value: onnx.ValueInfoProto) -> list[int | str | None]:
    result: list[int | str | None] = []
    for dim in value.type.tensor_type.shape.dim:
        if dim.HasField("dim_value"):
            result.append(dim.dim_value)
        elif dim.HasField("dim_param"):
            result.append(dim.dim_param)
        else:
            result.append(None)
    return result


def choose_source(root: Path) -> Path:
    candidates = list(root.rglob("rife_v4.6.onnx"))
    # The release may also contain representation-v2 models. MPCVR's current
    # 11-channel ABI intentionally targets the original `rife/` representation.
    v1 = [p for p in candidates if "rife_v2" not in {part.lower() for part in p.parts}]
    if len(v1) == 1:
        return v1[0]
    if not v1:
        raise RuntimeError("rife_v4.6.onnx was not found in the upstream archive")
    raise RuntimeError(f"ambiguous RIFE 4.6 source models: {[str(p) for p in v1]}")


def make_calibration_input(input_name: str, destination: Path) -> None:
    """Write one deterministic, structurally valid RIFE inference sample.

    AutoCast normally synthesizes inputs from model metadata. RIFE's H/W are
    dynamic, so the automatic generator resolves them to zero and ONNX Runtime
    cannot execute the reference graph. Supplying a representative padded video
    size keeps the exported model dynamic while giving the precision classifier
    a real inference sample.
    """

    height = CALIBRATION_HEIGHT
    width = CALIBRATION_WIDTH
    y = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None]
    x = np.linspace(0.0, 1.0, width, dtype=np.float32)[None, :]

    tensor = np.empty((1, 11, height, width), dtype=np.float32)

    # Two different but correlated normalized RGB frames. This is more
    # representative than unconstrained random noise and exercises motion/blend
    # paths without requiring copyrighted calibration media.
    tensor[0, 0] = x
    tensor[0, 1] = y
    tensor[0, 2] = 0.65 * x + 0.35 * y
    tensor[0, 3] = np.clip(x + 0.025, 0.0, 1.0)
    tensor[0, 4] = np.clip(y + 0.015, 0.0, 1.0)
    tensor[0, 5] = np.clip(0.65 * x + 0.35 * y + 0.02, 0.0, 1.0)

    tensor[0, 6].fill(0.5)
    tensor[0, 7] = np.broadcast_to(x * 2.0 - 1.0, (height, width))
    tensor[0, 8] = np.broadcast_to(y * 2.0 - 1.0, (height, width))
    tensor[0, 9].fill(2.0 / (width - 1))
    tensor[0, 10].fill(2.0 / (height - 1))

    np.savez(destination, **{input_name: tensor})


def validate_contract(model: onnx.ModelProto) -> dict[str, object]:
    onnx.checker.check_model(model)
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise RuntimeError(
            f"expected one input and one output, got {len(model.graph.input)} and {len(model.graph.output)}"
        )

    input_info = model.graph.input[0]
    output_info = model.graph.output[0]
    input_shape = tensor_shape(input_info)
    output_shape = tensor_shape(output_info)
    if len(input_shape) != 4 or input_shape[1] != 11:
        raise RuntimeError(f"unexpected RIFE input shape: {input_shape}")
    if len(output_shape) != 4 or output_shape[1] != 3:
        raise RuntimeError(f"unexpected RIFE output shape: {output_shape}")

    fp16 = onnx.TensorProto.FLOAT16
    input_type = input_info.type.tensor_type.elem_type
    output_type = output_info.type.tensor_type.elem_type
    if input_type != fp16 or output_type != fp16:
        raise RuntimeError(
            f"AutoCast must expose FP16 public I/O; got input type {input_type}, output type {output_type}"
        )

    return {
        "input_name": input_info.name,
        "input_shape": input_shape,
        "input_type": "FLOAT16",
        "output_name": output_info.name,
        "output_shape": output_shape,
        "output_type": "FLOAT16",
        "opset": max((entry.version for entry in model.opset_import if entry.domain in ("", "ai.onnx")), default=0),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path, help="upstream vs-mlrt rife_v8.7z")
    parser.add_argument("output", type=Path, help="destination rife_v4.6.onnx")
    args = parser.parse_args()

    archive = args.archive.resolve()
    output = args.output.resolve()
    if not archive.is_file():
        parser.error(f"archive not found: {archive}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mpcvr-rife-model-") as temp_name:
        temp = Path(temp_name)
        with py7zr.SevenZipFile(archive, mode="r") as package:
            package.extractall(path=temp)
        source = choose_source(temp)

        original = onnx.load(source)
        original_contract = {
            "input_shape": tensor_shape(original.graph.input[0]),
            "output_shape": tensor_shape(original.graph.output[0]),
        }
        if len(original_contract["input_shape"]) != 4 or original_contract["input_shape"][1] != 11:
            raise RuntimeError(f"upstream model no longer matches the 11-channel RIFE ABI: {original_contract}")

        calibration = temp / "rife_v4.6-calibration.npz"
        make_calibration_input(original.graph.input[0].name, calibration)

        converted = convert_to_mixed_precision(
            onnx_path=str(source),
            low_precision_type="fp16",
            keep_io_types=False,
            calibration_data=str(calibration),
            providers=["cpu"],
            use_standalone_type_inference=True,
        )
        contract = validate_contract(converted)

        staged = temp / "rife_v4.6.mixed-fp16.onnx"
        onnx.save(converted, staged)
        # Reload after serialization so the artifact itself—not only the in-memory
        # graph—is guaranteed to satisfy the runtime contract.
        contract = validate_contract(onnx.load(staged))
        shutil.copyfile(staged, output)

    metadata = {
        "source_archive_sha256": sha256(archive),
        "output_sha256": sha256(output),
        "output_bytes": output.stat().st_size,
        "contract": contract,
        "conversion": "NVIDIA Model Optimizer AutoCast fp16",
        "calibration_shape": [1, 11, CALIBRATION_HEIGHT, CALIBRATION_WIDTH],
    }
    metadata_path = output.with_name("model-metadata.json")
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
