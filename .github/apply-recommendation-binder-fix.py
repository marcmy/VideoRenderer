from pathlib import Path

path = Path('tools/UnifiedSetup/MpcvrSetup.Recommendations.psm1')
text = path.read_text(encoding='utf-8')
old = """        [Parameter(Mandatory)]
        [System.Collections.Generic.List[object]]$Candidates,
"""
new = """        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[object]]$Candidates,
"""
if text.count(old) != 1:
    raise RuntimeError(f'Expected one Candidates binder anchor, found {text.count(old)}')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
