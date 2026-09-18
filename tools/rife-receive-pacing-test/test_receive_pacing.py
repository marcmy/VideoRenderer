from pathlib import Path


repo = Path(__file__).resolve().parents[2]
source = (repo / "Source" / "VideoRenderer.cpp").read_text(encoding="utf-8")

submit_marker = "rifeSubmitted = m_RifePipeline->SubmitSample("
start_marker = "\n\tif (rifeSubmitted) {\n"
end_marker = "\n\t// A full source pool, device transition, or source-preparation failure"

submit = source.find(submit_marker)
assert submit != -1, "RIFE submission call was not found"

start = source.find(start_marker, submit)
assert start != -1, "RIFE success branch was not found"

end = source.find(end_marker, start)
assert end != -1, "RIFE success branch end was not found"

branch = source[start:end]

lead_wait_pos = branch.find("WaitForStreamTime(rtSourceStart - rifeFrameDuration)")
fallback_wait_pos = branch.find("WaitForRenderTime()")
clear_pos = branch.find("ClearPendingSample()")
cancel_pos = branch.find("CancelNotification()")

assert lead_wait_pos != -1, (
    "successful RIFE submission must pace one source frame early to provide bounded inference lookahead"
)
assert "m_pMediaSample->GetTime(&rtSourceStart, &rtSourceEnd)" in branch, (
    "bounded lookahead pacing must derive from the submitted sample timestamp"
)
assert fallback_wait_pos != -1, "untimed RIFE samples must retain the ordinary renderer pacing fallback"
assert clear_pos != -1 and lead_wait_pos < clear_pos, (
    "RIFE lookahead pacing must occur before the pending sample is cleared"
)
assert cancel_pos == -1 or cancel_pos > lead_wait_pos, (
    "the timing notification must not be cancelled before RIFE lookahead pacing"
)

print("RIFE receive pacing source-contract test passed")
