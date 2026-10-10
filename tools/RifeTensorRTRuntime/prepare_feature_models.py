#!/usr/bin/env python3
"""Split the pinned newer RIFE exports at their timestep-independent encoder.

Original mixed-precision models are never rewritten. Hash-qualified sidecars
have a six-channel image-only encoder and the original synthesis graph with
one additional feature input. Older exports without this encoder are skipped.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper
from onnx.utils import Extractor


def split_model(path: Path, destination: Path) -> dict:
    original_hash = hashlib.sha256(path.read_bytes()).hexdigest()
    model = onnx.shape_inference.infer_shapes(onnx.load(path), data_prop=True)
    encoder = [n for n in model.graph.node if n.name.startswith('/encode/')]
    if not encoder:
        return {'model': path.name, 'supported': False, 'reason': 'No separate image encoder'}
    # Fail closed if an upstream export changes the topology/precision.
    last = encoder[-1]
    if last.op_type != 'ConvTranspose' or len(last.output) != 1:
        raise ValueError('Unexpected encoder boundary')
    split = next(n for n in model.graph.node if n.name == '/Split')
    axes = {a.name: onnx.helper.get_attribute_value(a) for a in split.attribute}
    constants = {v.name: numpy_helper.to_array(v) for v in model.graph.initializer}
    for node in model.graph.node:
        if node.op_type == 'Constant':
            constants[node.output[0]] = numpy_helper.to_array(next(a.t for a in node.attribute if a.name == 'value'))
    lengths = constants[split.input[1]].tolist()
    if axes.get('axis') != 1 or lengths[0] != 6 or sum(lengths) != 11:
        raise ValueError('Encoder must consume only the first six RGB channels')
    images = split.output[0]
    feature = last.output[0]
    # The pinned AutoCast exports carry zero placeholders on this internal
    # value. They are dynamic image axes, not zero-sized feature inputs.
    for value in model.graph.value_info:
        if value.name == feature:
            for index, dim in enumerate(value.type.tensor_type.shape.dim):
                if index >= 2 and (not dim.HasField('dim_value') or dim.dim_value == 0):
                    dim.ClearField('dim_value')
                    dim.dim_param = ['H', 'W'][index - 2]
    extractor = Extractor(model)
    feature_model = extractor.extract_model([images], [feature])
    synthesis = extractor.extract_model([model.graph.input[0].name, feature], [model.graph.output[0].name])
    assert not any(n.name.startswith('/encode/') for n in synthesis.graph.node)
    assert feature_model.graph.input[0].type.tensor_type.shape.dim[1].dim_value == 6
    assert feature_model.graph.input[0].type.tensor_type.elem_type == onnx.TensorProto.FLOAT16
    assert feature_model.graph.output[0].type.tensor_type.elem_type == onnx.TensorProto.FLOAT16
    for stage in [feature_model, synthesis]:
        # Shape inference is needed only to find the cut tensors. AutoCast's
        # intermediate zero placeholders are not runtime shape constraints;
        # do not pass those inferred annotations into TensorRT's optimizer.
        del stage.graph.value_info[:]
        for value in list(stage.graph.input) + list(stage.graph.output):
            for index, dim in enumerate(value.type.tensor_type.shape.dim):
                if index >= 2:
                    dim.ClearField('dim_value')
                    dim.dim_param = value.name + ('_height' if index == 2 else '_width')
        onnx.checker.check_model(stage)
    destination.mkdir(parents=True, exist_ok=True)
    prefix = path.stem + '_' + original_hash[:16]
    feature_file = destination / (prefix + '.features.onnx')
    synthesis_file = destination / (prefix + '.synthesis.onnx')
    onnx.save(feature_model, feature_file)
    onnx.save(synthesis, synthesis_file)
    return {'model': path.name, 'supported': True, 'originalSha256': original_hash,
            'featureFile': feature_file.name, 'synthesisFile': synthesis_file.name,
            'featureSha256': hashlib.sha256(feature_file.read_bytes()).hexdigest(),
            'synthesisSha256': hashlib.sha256(synthesis_file.read_bytes()).hexdigest(),
            'imagesInput': images, 'featureTensor': feature}


def verify(path: Path, destination: Path, result: dict) -> list[dict]:
    import onnxruntime as ort
    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    options.intra_op_num_threads = 2
    sessions = [ort.InferenceSession(str(p), options, providers=['CPUExecutionProvider'])
                for p in [path, destination / result['featureFile'], destination / result['synthesisFile']]]
    whole, features, synthesis = sessions
    rng = np.random.default_rng(2833)
    cases = []
    for width, height in [(128, 128), (256, 128), (128, 256)]:
        tensor = rng.random((1, 11, height, width), dtype=np.float32).astype(np.float16)
        # Reuse the exact same encoder output across three target times.
        encoded = features.run(None, {features.get_inputs()[0].name: tensor[:, :6].copy()})[0]
        for timestep in [0.25, 0.5, 0.75]:
            tensor[:, 6].fill(timestep)
            expected = whole.run(None, {whole.get_inputs()[0].name: tensor})[0]
            actual = synthesis.run(None, {whole.get_inputs()[0].name: tensor, result['featureTensor']: encoded})[0]
            difference = float(np.max(np.abs(actual.astype(np.float32) - expected.astype(np.float32))))
            if not np.array_equal(actual, expected):
                raise AssertionError(f'{path.name} split mismatch {width}x{height} t={timestep}: {difference}')
            cases.append({'width': width, 'height': height, 'timestep': timestep, 'maxError': difference})
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    results = []
    for path in sorted(args.models.glob('rife_v*.onnx')):
        if '.features.' in path.name or '.synthesis.' in path.name:
            continue
        result = split_model(path, args.output)
        if result['supported'] and args.verify:
            result['cpuParity'] = verify(path, args.output, result)
        results.append(result)
        print(path.name, 'split and verified' if 'cpuParity' in result else result.get('reason', 'split'), flush=True)
    (args.output / 'feature-models.json').write_text(json.dumps({'schemaVersion': 1, 'models': results}, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
