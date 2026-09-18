from pathlib import Path

repo = Path(__file__).resolve().parents[2]
source = (repo / "Source" / "RifePlaybackPipeline.cpp").read_text(encoding="utf-8")


def function_body(text: str, marker: str) -> str:
    start = text.find(marker)
    assert start != -1, f"{marker} implementation was not found"
    brace = text.find("{", start)
    assert brace != -1, f"{marker} implementation body was not found"
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index + 1]
    raise AssertionError(f"{marker} implementation end was not found")


process_pair = function_body(source, "void ProcessPair(")

guard_pos = process_pair.find("hasTimelySyntheticTarget")
nvof_pos = process_pair.find("nvofDetector.BeginAnalyze(")
image_pos = process_pair.find("DetectImageSceneCut(first, second)")
late_pos = process_pair.find("if (IsLate(second, target.presentationTime))")

assert guard_pos != -1, (
    "ProcessPair must detect whether any synthetic target is still timely before running scene detection"
)
assert nvof_pos != -1 and guard_pos < nvof_pos and late_pos != -1 and late_pos < nvof_pos, (
    "late synthetic targets must be filtered before NVOF analysis is launched"
)
assert image_pos != -1 and guard_pos < image_pos, (
    "image scene detection must remain gated by the timely-synthetic-target check"
)

print("RIFE catch-up source-contract test passed")
