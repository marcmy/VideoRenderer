from pathlib import Path

repo = Path(__file__).resolve().parents[2]
header = (repo / "Source" / "NvidiaSceneChangeDetector.h").read_text(encoding="utf-8")
detector = (repo / "Source" / "NvidiaSceneChangeDetector.cpp").read_text(encoding="utf-8")
pipeline = (repo / "Source" / "RifePlaybackPipeline.cpp").read_text(encoding="utf-8")


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


assert "bool BeginAnalyze(" in header and "bool FinishAnalyze(" in header, (
    "NVOF scene analysis must expose launch and completion phases so OFA can overlap RIFE inference"
)
assert "constexpr UINT kAnalysisStride = 2;" in detector, (
    "scene-cut statistics should subsample the full bidirectional 4x4 flow field instead of reducing every vector"
)

process_pair = function_body(pipeline, "void ProcessPairJob(")
begin_pos = process_pair.find("nvofDetector.BeginAnalyze(")
generate_pos = process_pair.find("GenerateRife(")
finish_pos = process_pair.find("nvofDetector.FinishAnalyze(")
assert begin_pos != -1 and generate_pos != -1 and finish_pos != -1, (
    "the uncontended path must launch NVOF, run RIFE, then finish the NVOF scene decision"
)
assert begin_pos < generate_pos < finish_pos, (
    "bidirectional NVOF must overlap the first RIFE inference rather than serialize before it"
)
assert "DetectImageSceneCut(workerState.imageDetector, first, second)" in process_pair[finish_pos:], (
    "an NVOF completion failure must retain image-comparison fallback"
)
assert "std::mutex nvofMutex;" in pipeline and "CNvidiaSceneChangeDetector nvofDetector;" in pipeline, (
    "parallel TensorRT workers must share one serialized NVOF detector/session"
)
assert "std::try_to_lock" in process_pair, (
    "NVOF contention must not block a parallel worker before it submits TensorRT inference"
)
generate_after_begin = process_pair.find("GenerateRife(", begin_pos)
serialized_analyze = process_pair.find("nvofDetector.Analyze(", generate_after_begin)
assert serialized_analyze > generate_after_begin, (
    "a worker that loses the NVOF lock must run RIFE first and serialize scene analysis afterward"
)
assert "workerState.nvofDetector" not in pipeline, (
    "per-worker NVOF sessions reintroduce concurrent OFA/D3D11 driver stalls"
)

print("RIFE NVOF overlap source-contract test passed")
