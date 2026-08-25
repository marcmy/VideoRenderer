# Repository Agent Instructions

## GitHub Actions

- Do not commit disposable, one-off, repair, probe, or trigger-only workflow files under `.github/workflows/` unless the user explicitly requests a permanent new workflow.
- Do not add workflows named or patterned like `one-time-*`, `*-trigger.yml`, temporary repair workflows, or temporary validation workflows for a single task.
- For one-off validation, prefer existing workflows, `workflow_dispatch`, local build/test scripts, or manual `gh workflow run` invocations.
- If a temporary workflow is absolutely unavoidable during development, remove it before finalizing the task or opening/updating a pull request.
- Reuse or extend an existing durable workflow when CI coverage genuinely needs to change.
