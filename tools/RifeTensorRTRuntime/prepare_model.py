#!/usr/bin/env python3
"""Prepare mixed-precision RIFE ONNX models consumed by MPCVR.

The renderer's TensorRT ABI accepts the original Practical-RIFE-derived
11-channel representation. This script extracts one selected model from a
pinned AmusementClub/vs-mlrt archive, converts it with NVIDIA Model Optimizer
AutoCast, validates the public tensor contract, and can assemble the release
manifest for the complete model bundle.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import tempfile
from pathlib import Path


CALIBRATION_WIDTH = 1024
CALIBRATION_HEIGHT = 576
MODEL_MANIFEST_SCHEMA_VERSION = 2
MODEL_PRECISION = "mixed-fp16-fp32"
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")

MODEL_SPECS = {
    "4.4": {
        "model": "RIFE 4.4",
        "file": "rife_v4.4.onnx",
        "sourceRelease": "model-20220923",
        "sourceMember": "rife/rife_v4.4.onnx",
    },
    "4.6": {
        "model": "RIFE 4.6",
        "file": "rife_v4.6.onnx",
        "sourceRelease": "model-20220923",
        "sourceMember": "rife/rife_v4.6.onnx",
    },
    "4.15-lite": {
        "model": "RIFE 4.15 Lite",
        "file": "rife_v4.15_lite.onnx",
        "sourceRelease": "external-models",
        "sourceMember": "rife/rife_v4.15_lite.onnx",
    },
}
MODEL_ORDER = tuple(MODEL_SPECS)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tensor_shape(value: object) -> list[int | str | None]:
    result: list[int | str | None] = []
    for dim in value.type.tensor_type.shape.dim:
        if dim.HasField("dim_value"):
            result.append(dim.dim_value)
        elif dim.HasField("dim_param"):
            result.append(dim.dim_param)
        else:
            result.append(None)
    return result


def choose_source(root: Path, model_key: str) -> Path:
    spec = MODEL_SPECS[model_key]
    candidates = list(root.rglob(str(spec["file"])))
    v1 = [p for p in candidates if "rife_v2" not in {part.lower() for part in p.parts}]
    if len(v1) == 1:
        return v1[0]
    if not v1:
        raise RuntimeError(f"{spec['file']} was not found in the upstream archive")
    raise RuntimeError(f"ambiguous {spec['model']} source models: {[str(p) for p in v1]}")


def make_calibration_input(input_name: str, destination: Path) -> None:
    import numpy as np

    height = CALIBRATION_HEIGHT
    width = CALIBRATION_WIDTH
    y = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None]
    x = np.linspace(0.0, 1.0, width, dtype=np.float32)[None, :]
    tensor = np.empty((1, 11, height, width), dtype=np.float32)

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


def validate_source_contract(model: object) -> dict[str, object]:
    import onnx

    onnx.checker.check_model(model)
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise RuntimeError(
            f"expected one input and one output, got {len(model.graph.input)} and {len(model.graph.output)}"
        )
    input_shape = tensor_shape(model.graph.input[0])
    output_shape = tensor_shape(model.graph.output[0])
    if len(input_shape) != 4 or input_shape[0] != 1 or input_shape[1] != 11:
        raise RuntimeError(f"upstream model no longer matches the 11-channel RIFE ABI: {input_shape}")
    if len(output_shape) != 4 or output_shape[0] != 1 or output_shape[1] != 3:
        raise RuntimeError(f"upstream model no longer matches the 3-channel RIFE output ABI: {output_shape}")

    return {
        "input_shape": input_shape,
        "output_shape": output_shape,
        "opset": max(
            (entry.version for entry in model.opset_import if entry.domain in ("", "ai.onnx")),
            default=0,
        ),
    }


def validate_contract(model: object) -> dict[str, object]:
    import onnx

    source_contract = validate_source_contract(model)
    input_info = model.graph.input[0]
    output_info = model.graph.output[0]
    fp16 = onnx.TensorProto.FLOAT16
    input_type = input_info.type.tensor_type.elem_type
    output_type = output_info.type.tensor_type.elem_type
    if input_type != fp16 or output_type != fp16:
        raise RuntimeError(
            f"AutoCast must expose FP16 public I/O; got input type {input_type}, output type {output_type}"
        )

    return {
        "input_name": input_info.name,
        "input_shape": source_contract["input_shape"],
        "input_type": "FLOAT16",
        "output_name": output_info.name,
        "output_shape": source_contract["output_shape"],
        "output_type": "FLOAT16",
        "opset": source_contract["opset"],
    }


def expected_model_entry(model_key: str, archive: Path) -> dict[str, object]:
    spec = MODEL_SPECS[model_key]
    return {
        "model": spec["model"],
        "file": spec["file"],
        "sourceRelease": spec["sourceRelease"],
        "sourceArchive": archive.name,
        "sourceArchiveSha256": sha256(archive),
        "sourceModel": spec["sourceMember"],
        "input": [1, 11, "H", "W"],
        "output": [1, 3, "H", "W"],
        "precision": MODEL_PRECISION,
    }


def validate_release_manifest(manifest: object) -> dict[str, object]:
    if not isinstance(manifest, dict):
        raise RuntimeError("model manifest must be a JSON object")
    if set(manifest) != {"schemaVersion", "models"}:
        raise RuntimeError("model manifest must contain exactly schemaVersion and models")
    if manifest["schemaVersion"] != MODEL_MANIFEST_SCHEMA_VERSION:
        raise RuntimeError(
            f"model manifest schemaVersion must be {MODEL_MANIFEST_SCHEMA_VERSION}; "
            f"got {manifest['schemaVersion']!r}"
        )

    models = manifest["models"]
    if not isinstance(models, list) or len(models) != len(MODEL_ORDER):
        raise RuntimeError(f"model manifest must contain exactly {len(MODEL_ORDER)} models")

    expected_names = [str(MODEL_SPECS[key]["model"]) for key in MODEL_ORDER]
    if [entry.get("model") if isinstance(entry, dict) else None for entry in models] != expected_names:
        raise RuntimeError(f"model manifest models must be ordered as {expected_names}")

    required_fields = {
        "model",
        "file",
        "sourceRelease",
        "sourceArchive",
        "sourceArchiveSha256",
        "sourceModel",
        "input",
        "output",
        "precision",
    }
    for model_key, entry in zip(MODEL_ORDER, models, strict=True):
        if not isinstance(entry, dict) or set(entry) != required_fields:
            raise RuntimeError(f"{MODEL_SPECS[model_key]['model']} manifest entry has invalid fields")
        spec = MODEL_SPECS[model_key]
        expected = {
            "model": spec["model"],
            "file": spec["file"],
            "sourceRelease": spec["sourceRelease"],
            "sourceModel": spec["sourceMember"],
            "input": [1, 11, "H", "W"],
            "output": [1, 3, "H", "W"],
            "precision": MODEL_PRECISION,
        }
        for key, value in expected.items():
            if entry[key] != value:
                raise RuntimeError(
                    f"{spec['model']} manifest field {key!r} must be {value!r}; got {entry[key]!r}"
                )
        if not isinstance(entry["sourceArchive"], str) or not entry["sourceArchive"]:
            raise RuntimeError(f"{spec['model']} sourceArchive must be a non-empty string")
        source_hash = entry["sourceArchiveSha256"]
        if not isinstance(source_hash, str) or not SHA256_PATTERN.fullmatch(source_hash):
            raise RuntimeError(f"{spec['model']} sourceArchiveSha256 must be a lowercase SHA-256 digest")

    return manifest


def parse_source_archives(values: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        model_key, separator, path_text = value.partition("=")
        if not separator or model_key not in MODEL_SPECS or not path_text:
            raise RuntimeError(
                "--source-archive entries must use MODEL=PATH with MODEL one of "
                + ", ".join(MODEL_ORDER)
            )
        path = Path(path_text).resolve()
        if not path.is_file():
            raise RuntimeError(f"source archive not found for {model_key}: {path}")
        result[model_key] = path

    missing = [key for key in MODEL_ORDER if key not in result]
    if missing:
        raise RuntimeError(f"missing source archive mappings for: {missing}")
    return result


def write_release_manifest(output_dir: Path, source_archives: dict[str, Path]) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    for key in MODEL_ORDER:
        model_path = output_dir / str(MODEL_SPECS[key]["file"])
        if not model_path.is_file():
            raise RuntimeError(f"converted model is missing: {model_path}")

    manifest = validate_release_manifest(
        {
            "schemaVersion": MODEL_MANIFEST_SCHEMA_VERSION,
            "models": [expected_model_entry(key, source_archives[key]) for key in MODEL_ORDER],
        }
    )
    manifest_path = output_dir / "model-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest_path


def convert_model(model_key: str, archive: Path, output: Path) -> dict[str, object]:
    import onnx
    import py7zr
    from modelopt.onnx.autocast import convert_to_mixed_precision

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"mpcvr-rife-{model_key}-") as temp_name:
        temp = Path(temp_name)
        with py7zr.SevenZipFile(archive, mode="r") as package:
            package.extractall(path=temp)
        source = choose_source(temp, model_key)
        original = onnx.load(source)
        source_contract = validate_source_contract(original)

        calibration = temp / f"{output.stem}-calibration.npz"
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

        staged = temp / f"{output.stem}.mixed-fp16.onnx"
        onnx.save(converted, staged)
        contract = validate_contract(onnx.load(staged))
        shutil.copyfile(staged, output)

    metadata = {
        "model": MODEL_SPECS[model_key]["model"],
        "source_archive_sha256": sha256(archive),
        "source_contract": source_contract,
        "output_sha256": sha256(output),
        "output_bytes": output.stat().st_size,
        "contract": contract,
        "conversion": "NVIDIA Model Optimizer AutoCast fp16",
        "calibration_shape": [1, 11, CALIBRATION_HEIGHT, CALIBRATION_WIDTH],
    }
    metadata_path = output.with_suffix(".metadata.json")
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", nargs="?", type=Path, help="upstream vs-mlrt model archive")
    parser.add_argument("output", nargs="?", type=Path, help="destination mixed-precision ONNX")
    parser.add_argument("--model", choices=MODEL_ORDER, default="4.6", help="model variant to convert")
    parser.add_argument(
        "--validate-manifest",
        type=Path,
        metavar="PATH",
        help="validate a release model-manifest.json without loading ONNX/model conversion dependencies",
    )
    parser.add_argument(
        "--write-manifest",
        type=Path,
        metavar="OUTPUT_DIR",
        help="write the three-model release manifest into OUTPUT_DIR",
    )
    parser.add_argument(
        "--source-archive",
        action="append",
        default=[],
        metavar="MODEL=PATH",
        help="source archive mapping used with --write-manifest; repeat for every model",
    )
    args = parser.parse_args()

    if args.validate_manifest is not None:
        if args.archive is not None or args.output is not None or args.write_manifest is not None:
            parser.error("--validate-manifest cannot be combined with conversion or --write-manifest")
        manifest_path = args.validate_manifest.resolve()
        if not manifest_path.is_file():
            parser.error(f"manifest not found: {manifest_path}")
        validate_release_manifest(json.loads(manifest_path.read_text(encoding="utf-8-sig")))
        print(f"model manifest validation passed: {manifest_path}")
        return 0

    if args.write_manifest is not None:
        if args.archive is not None or args.output is not None:
            parser.error("--write-manifest cannot be combined with archive/output conversion arguments")
        try:
            source_archives = parse_source_archives(args.source_archive)
            manifest_path = write_release_manifest(args.write_manifest.resolve(), source_archives)
        except RuntimeError as error:
            parser.error(str(error))
        print(f"model manifest written: {manifest_path}")
        return 0

    if args.source_archive:
        parser.error("--source-archive requires --write-manifest")
    if args.archive is None or args.output is None:
        parser.error("archive and output are required unless a manifest operation is used")

    archive = args.archive.resolve()
    output = args.output.resolve()
    if not archive.is_file():
        parser.error(f"archive not found: {archive}")

    metadata = convert_model(args.model, archive, output)
    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
