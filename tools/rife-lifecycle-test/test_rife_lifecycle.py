from pathlib import Path


repo = Path(__file__).resolve().parents[2]

renderer = (repo / "Source" / "VideoRenderer.cpp").read_text(encoding="utf-8")
legacy_renderer = (repo / "Source" / "VideoRendererLegacy.inl").read_text(encoding="utf-8")
dx11_header = (repo / "Source" / "DX11VideoProcessor.h").read_text(encoding="utf-8")
dx11_processor = (repo / "Source" / "DX11VideoProcessor.cpp").read_text(encoding="utf-8")
rife_pipeline = (repo / "Source" / "RifePlaybackPipeline.cpp").read_text(encoding="utf-8")
rife_bridge = (repo / "Source" / "RifeDX11Bridge.cpp").read_text(encoding="utf-8")
rife_runtime = (repo / "tools" / "RifeTensorRTRuntime" / "RifeTensorRTRuntime.cpp").read_text(encoding="utf-8")


def function_body(source: str, marker: str) -> str:
    start = source.find(marker)
    assert start != -1, f"{marker} implementation was not found"
    brace = source.find("{", start)
    assert brace != -1, f"{marker} implementation body was not found"
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"{marker} implementation end was not found")


assert "RifeAlignedDimension" in dx11_header, (
    "RIFE source surfaces must be aligned to the model's 32-pixel spatial requirement"
)

prepared_start = dx11_processor.find("if (m_pFrameInterpolationTexture && m_pFrameInterpolationView)")
assert prepared_start != -1, "prepared-frame presentation branch was not found"
prepared_end = dx11_processor.find("\n\tHRESULT hr = S_OK;", prepared_start)
assert prepared_end != -1, "prepared-frame presentation branch end was not found"
prepared_branch = dx11_processor[prepared_start:prepared_end]
assert "GetRifeContentSize()" in prepared_branch and "ResizeShaderPass" in prepared_branch, (
    "aligned RIFE surfaces must crop padding and scale the content into the presentation rectangle"
)

assert "static bool RifeFramesCompatible" in rife_pipeline and "RifeFramesCompatible(*previous, *currentFrame)" in rife_pipeline, (
    "the worker must never pair RIFE source textures from different device/size generations"
)

assert "AcquireRifePresentationSurface" in dx11_header and "AcquireRifePresentationSurface" in rife_bridge, (
    "generated RIFE frames need a presentation surface whose retirement query owns texture reuse"
)
generate_rife = function_body(rife_pipeline, "bool GenerateRife(")
assert "runtime->Interpolate(contextIndex, first, second," in generate_rife
assert "outputTexture" not in generate_rife, (
    "parallel RIFE inference must write into a context-owned texture instead of the shared scene-blend texture"
)
submit = function_body(rife_pipeline, "bool Submit(")
assert "CopyResource(inferenceAsFirst, texture)" in submit and "CopyResource(inferenceAsSecond, texture)" in submit, (
    "parallel source frames must stage role-specific CUDA input copies immediately after source preparation"
)
assert "frame.inferenceAsFirst = inferenceAsFirst" in submit and "frame.inferenceAsSecond = inferenceAsSecond" in submit, (
    "the role-specific CUDA inputs must follow the source frame through adjacent pair jobs"
)
assert "InputResourceClaim inputResourceClaim" in rife_runtime, (
    "the runtime must protect only genuinely shared CUDA graphics resources"
)
assert "inputResourceClaim.Release()" in rife_runtime and "inputReleasedEvent" in rife_runtime, (
    "shared A/B inputs must be released after map/pack/unmap instead of serializing full TensorRT requests"
)
assert "m_inputPackMutex" not in rife_runtime, (
    "pre-staged input copies must not be serialized by a process-wide input-pack mutex"
)
assert "cudaStreamBeginCapture" in rife_runtime and "cudaGraphLaunch" in rife_runtime, (
    "TensorRT submission should use a per-context CUDA graph when capture is supported"
)
assert "state->context->setTensorAddress" in rife_runtime and "PrepareContexts()" in rife_runtime, (
    "fixed per-context tensor buffers should be bound during runtime preparation"
)
acquire_output = function_body(rife_pipeline, "ID3D11Texture2D* AcquireInferenceOutput(")
assert "workerState.inferenceOutputs" in acquire_output and "CreateBgraTexture" in acquire_output, (
    "CUDA outputs must come from a stable per-context pool instead of creating a registered texture every frame"
)
process_pair = function_body(rife_pipeline, "void ProcessPairJob(")
assert "first.inferenceAsFirst" in process_pair and "second.inferenceAsSecond" in process_pair
assert "second.inferenceAsFirst" in process_pair, (
    "single-context mode must fall back to the one staged input copy"
)
assert "AcquireInferenceOutput" in process_pair and "result.generated" in process_pair, (
    "parallel pair workers must use pre-staged CUDA inputs and retain results until ordered presentation"
)
present_pair = function_body(rife_pipeline, "bool PresentPairJob(")
assert "QueueTexture(" in present_pair and "result.generated" in present_pair, (
    "completed pair jobs must enter presentation through the ordered source-copy path"
)
worker_main = function_body(rife_pipeline, "void WorkerMain(")
assert "pendingPairs.front()" in worker_main and "PresentPairJob(*job)" in worker_main, (
    "parallel pair completion must be drained in source-pair order"
)
adopt_frame = function_body(rife_pipeline, "SourceFramePtr AdoptFrame(")
assert "ReleaseFrame(*owned)" in adopt_frame, (
    "shared source frames must retain their source-pool slot until every adjacent pair job releases them"
)

assert "ForgetRifeSettings(this)" in legacy_renderer, (
    "destroying a renderer must remove its address from the RIFE settings-load tracker"
)
assert "g_rifeSettingsLoaded.erase(renderer)" in renderer, (
    "RIFE settings tracker cleanup must actually erase the dead renderer address"
)

destructor = function_body(legacy_renderer, "CMpcVideoRenderer::~CMpcVideoRenderer(")
assert "m_RifePipeline.reset()" in destructor, (
    "renderer teardown must join/destroy the RIFE worker before destroying the video processor"
)
assert destructor.find("m_RifePipeline.reset()") < destructor.find("m_VideoProcessor.reset()"), (
    "the RIFE worker must not retain raw processor references after the processor is destroyed"
)

for transition in ("NewSegment", "BeginFlush", "Stop"):
    marker = f"CMpcVideoRenderer::{transition}("
    body = function_body(legacy_renderer, marker)
    assert "m_RifePipeline->Reset()" in body, (
        f"{transition} must invalidate queued/in-flight RIFE work immediately"
    )

print("RIFE lifecycle/source-contract test passed")
