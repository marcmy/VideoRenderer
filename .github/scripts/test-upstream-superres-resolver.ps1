$ErrorActionPreference = 'Stop'
$resolver = Join-Path $PSScriptRoot 'resolve-upstream-superres.ps1'
$fixture = @'
// unchanged prefix
FORK_MARKER
	auto superRes = (m_bVPScaling && m_iMaxineOperation == MAXINE_OPERATION_Disabled)
		? m_iVPSuperRes : SUPERRES_Disable;
SEPARATOR_MARKER
	int superRes = SUPERRES_Disable;
	if (m_bVPScaling && !(m_bACMEnabled && !m_bVPUseRTXVideoHDR && params.CDepth == 8 && m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM)) {
		superRes = m_iVPSuperRes;
	}

UPSTREAM_MARKER
// unchanged suffix
'@
$fixture = $fixture.Replace('FORK_MARKER', '<<<<<<< HEAD').Replace('SEPARATOR_MARKER', '=======').Replace('UPSTREAM_MARKER', '>>>>>>> upstream/master').Replace("`r`n", "`n")
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('mpcvr-sync-' + [guid]::NewGuid() + '.cpp')
$checks = 0
try {
    foreach ($newline in @("`n", "`r`n")) {
        foreach ($bom in @($false, $true)) {
            [IO.File]::WriteAllText($temporary, $fixture.Replace("`n", $newline), [Text.UTF8Encoding]::new($bom))
            if (-not (& $resolver -Path $temporary -Probe)) { throw 'Known hunk rejected.' }
            & $resolver -Path $temporary
            $actual = [IO.File]::ReadAllText($temporary)
            if (-not $actual.Contains('m_iMaxineOperation == MAXINE_OPERATION_Disabled') -or
                -not $actual.Contains('!(m_bACMEnabled && !m_bVPUseRTXVideoHDR && params.CDepth == 8') -or
                -not $actual.StartsWith('// unchanged prefix') -or -not $actual.EndsWith('// unchanged suffix') -or
                $actual.Contains('<<<<<<<')) { throw 'Resolution lost a required condition or unrelated text.' }
            $bytes = [IO.File]::ReadAllBytes($temporary)
            if (($bytes[0] -eq 0xef) -ne $bom) { throw 'UTF-8 BOM changed.' }
            if ($newline -eq "`r`n" -and $actual.Replace("`r`n", '').Contains("`n")) { throw 'CRLF changed.' }
            $checks++
        }
    }
    $unknown = @(
        $fixture.Replace('params.CDepth == 8', 'params.CDepth == 10'),
        $fixture.Replace('MAXINE_OPERATION_Disabled', 'MAXINE_OPERATION_Upscale'),
        ($fixture + "`n<<<<<<< HEAD`nunknown`n=======`nother`n>>>>>>> upstream/master"),
        ($fixture + "`n" + $fixture),
        '// no conflict'
    )
    foreach ($text in $unknown) {
        [IO.File]::WriteAllText($temporary, $text, [Text.UTF8Encoding]::new($false))
        $before = [Convert]::ToBase64String([IO.File]::ReadAllBytes($temporary))
        if (& $resolver -Path $temporary -Probe) { throw 'Unknown hunk accepted.' }
        $rejected = $false
        try { & $resolver -Path $temporary } catch { $rejected = $true }
        if (-not $rejected -or $before -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes($temporary))) {
            throw 'Unknown hunk was changed rather than rejected.'
        }
        $checks++
    }
    "PASS: $checks SuperRes resolver fixtures (CPU only)."
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
