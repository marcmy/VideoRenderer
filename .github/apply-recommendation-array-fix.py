from pathlib import Path

path = Path('tools/UnifiedSetup/MpcvrSetup.Recommendations.psm1')
text = path.read_text(encoding='utf-8')

old_function = '''function Add-MpcvrRecommendationCandidate {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[object]]$Candidates,
        [Parameter(Mandatory)]
        [string]$Id,
        [Parameter(Mandatory)]
        [string]$Title,
        [Parameter(Mandatory)]
        [string]$Reason,
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Settings,
        [bool]$RequiresCalibration = $true,
        [string]$Impact = 'Unknown until recalibrated'
    )

    if (@($Candidates | Where-Object { $_.Id -eq $Id }).Count -gt 0) {
        return
    }
    $Candidates.Add([pscustomobject]@{
        Id = $Id
        Title = $Title
        Reason = $Reason
        RequiresCalibration = $RequiresCalibration
        Impact = $Impact
        Settings = [pscustomobject](Copy-MpcvrSettingsDictionary -Settings $Settings)
    })
}
'''
new_function = '''function Add-MpcvrRecommendationCandidate {
    param(
        [Parameter(Mandatory)]
        [ref]$Candidates,
        [Parameter(Mandatory)]
        [string]$Id,
        [Parameter(Mandatory)]
        [string]$Title,
        [Parameter(Mandatory)]
        [string]$Reason,
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$Settings,
        [bool]$RequiresCalibration = $true,
        [string]$Impact = 'Unknown until recalibrated'
    )

    if (@($Candidates.Value | Where-Object { $_.Id -eq $Id }).Count -gt 0) {
        return
    }
    $Candidates.Value = @($Candidates.Value) + [pscustomobject]@{
        Id = $Id
        Title = $Title
        Reason = $Reason
        RequiresCalibration = $RequiresCalibration
        Impact = $Impact
        Settings = [pscustomobject](Copy-MpcvrSettingsDictionary -Settings $Settings)
    }
}
'''
if text.count(old_function) != 1:
    raise RuntimeError(f'Expected one candidate helper, found {text.count(old_function)}')
text = text.replace(old_function, new_function, 1)

replacements = {
    "$candidates = New-Object 'System.Collections.Generic.List[object]'": "$candidates = @()",
    '-Candidates $candidates `': '-Candidates ([ref]$candidates) `',
    '[array]::IndexOf([object[]]$candidates.ToArray(), $match[0])': '[array]::IndexOf([object[]]$candidates, $match[0])',
}
for old, new in replacements.items():
    count = text.count(old)
    if count == 0:
        raise RuntimeError(f'Missing expected recommendation anchor: {old}')
    text = text.replace(old, new)

path.write_text(text, encoding='utf-8')
