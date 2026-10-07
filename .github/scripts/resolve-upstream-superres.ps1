param(
    [string]$Path = 'Source/DX11VideoProcessor.cpp',
    [switch]$Probe
)
$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)
$bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf
$text = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path).Path)
$newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$expected = @(
    '<<<<<<< HEAD',
    "`tauto superRes = (m_bVPScaling && m_iMaxineOperation == MAXINE_OPERATION_Disabled)",
    "`t`t? m_iVPSuperRes : SUPERRES_Disable;",
    '=======',
    "`tint superRes = SUPERRES_Disable;",
    "`tif (m_bVPScaling && !(m_bACMEnabled && !m_bVPUseRTXVideoHDR && params.CDepth == 8 && m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM)) {",
    "`t`tsuperRes = m_iVPSuperRes;",
    "`t}",
    '',
    '>>>>>>> upstream/master'
) -join $newline
$matches = [regex]::Matches($text, [regex]::Escape($expected)).Count
$markers = [regex]::Matches($text, '(?m)^(<<<<<<<|=======|>>>>>>>)(?: |\r?$)').Count
$known = $matches -eq 1 -and $markers -eq 3
if ($Probe) { return $known }
if (-not $known) { throw 'SuperRes conflict is not the single reviewed hunk; refusing automatic resolution.' }
$replacement = @(
    "`tint superRes = SUPERRES_Disable;",
    "`tif (m_bVPScaling && m_iMaxineOperation == MAXINE_OPERATION_Disabled",
    "`t`t&& !(m_bACMEnabled && !m_bVPUseRTXVideoHDR && params.CDepth == 8 && m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM)) {",
    "`t`tsuperRes = m_iVPSuperRes;",
    "`t}",
    ''
) -join $newline
[IO.File]::WriteAllText((Resolve-Path -LiteralPath $Path).Path, $text.Replace($expected, $replacement), [Text.UTF8Encoding]::new($bom))
