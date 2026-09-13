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

wait_pos = branch.find("WaitForRenderTime()")
clear_pos = branch.find("ClearPendingSample()")
cancel_pos = branch.find("CancelNotification()")

assert wait_pos != -1, "successful RIFE submission must retain source-time pacing"
assert clear_pos != -1 and wait_pos < clear_pos, "RIFE pacing must occur before the pending sample is cleared"
assert cancel_pos == -1 or cancel_pos > wait_pos, "the timing notification must not be cancelled before RIFE pacing"

print("RIFE receive pacing source-contract test passed")
