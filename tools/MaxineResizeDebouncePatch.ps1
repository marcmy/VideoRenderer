$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$header = Join-Path $root 'Source/DX11VideoProcessor.h'
$cpp = Join-Path $root 'Source/DX11VideoProcessor.cpp'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Read-Normalized([string]$Path) {
    return [IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
}

function Replace-Exact([string]$Path, [string]$Old, [string]$New) {
    $text = Read-Normalized $Path
    $oldNormalized = $Old.Replace("`r`n", "`n")
    $newNormalized = $New.Replace("`r`n", "`n")
    if (-not $text.Contains($oldNormalized)) {
        throw "Expected source block not found in $Path"
    }
    [IO.File]::WriteAllText($Path, $text.Replace($oldNormalized, $newNormalized), $utf8NoBom)
}

if ((Read-Normalized $header).Contains('MaxineResizeDebounceMs')) {
    Write-Host 'Maxine resize debounce patch is already applied.'
    exit 0
}

Replace-Exact $header @'
	std::wstring m_strMaxinePipeline;
	std::wstring m_strMaxineRuntimeInfo;
	CNvidiaMaxineVSR m_MaxineVSR;
'@ @'
	std::wstring m_strMaxinePipeline;
	std::wstring m_strMaxineRuntimeInfo;
	bool m_bMaxineResizePending = false;
	ULONGLONG m_MaxineResizeLastChangeTick = 0;
	static constexpr ULONGLONG MaxineResizeDebounceMs = 250;
	CNvidiaMaxineVSR m_MaxineVSR;
'@

Replace-Exact $cpp @'
	if (m_iMaxineScale == MAXINE_SCALE_MatchOutput) {
		int dstWidth = dstRect.Width();
'@ @'
	if (m_iMaxineScale == MAXINE_SCALE_MatchOutput
			&& m_bMaxineResizePending && m_bMaxineVSRUsed
			&& m_MaxineVSRSize.cx > 0 && m_MaxineVSRSize.cy > 0) {
		// During interactive zoom, keep the current Maxine inference size stable.
		// MPCVR's final scaler follows dstRect immediately; Maxine is reloaded once
		// after the resize input has been quiet for MaxineResizeDebounceMs.
		targetWidth = static_cast<unsigned long long>(m_MaxineVSRSize.cx);
		targetHeight = static_cast<unsigned long long>(m_MaxineVSRSize.cy);
	}
	else if (m_iMaxineScale == MAXINE_SCALE_MatchOutput) {
		int dstWidth = dstRect.Width();
'@

Replace-Exact $cpp @'
void CDX11VideoProcessor::UpdateTexures()
{
	if (!m_srcWidth || !m_srcHeight) {
		return;
	}

	// D3D11 textures registered with CUDA must be unregistered before the
'@ @'
void CDX11VideoProcessor::UpdateTexures()
{
	if (!m_srcWidth || !m_srcHeight) {
		return;
	}

	// Any explicit texture rebuild supersedes a deferred interactive resize.
	m_bMaxineResizePending = false;
	m_MaxineResizeLastChangeTick = 0;

	// D3D11 textures registered with CUDA must be unregistered before the
'@

Replace-Exact $cpp @'
	const UINT numSteps = GetPostScaleSteps();
	CSize maxineTargetSize;
	bool maxineUpscaleNeeded = false;
	const bool canUseMaxineVSR = GetMaxineVSRTargetSize(dstRect, maxineTargetSize, maxineUpscaleNeeded);
'@ @'
	const UINT numSteps = GetPostScaleSteps();

	if (m_bMaxineResizePending && m_iMaxineScale == MAXINE_SCALE_MatchOutput
			&& GetTickCount64() - m_MaxineResizeLastChangeTick >= MaxineResizeDebounceMs) {
		// Reconfigure exactly once after interactive Video Frame Inc/Dec input stops.
		m_bMaxineResizePending = false;
		m_MaxineResizeLastChangeTick = 0;
		UpdateTexures();
	}

	CSize maxineTargetSize;
	bool maxineUpscaleNeeded = false;
	const bool canUseMaxineVSR = GetMaxineVSRTargetSize(dstRect, maxineTargetSize, maxineUpscaleNeeded);
'@

Replace-Exact $cpp @'
void CDX11VideoProcessor::SetVideoRect(const CRect& videoRect)
{
	m_videoRect = videoRect;
	UpdateRenderRect();

	// Fixed Maxine scales do not need texture recreation for presentation-only
	// zoom changes. Match-output mode intentionally follows the player size.
	CSize maxineTargetSize;
	bool maxineUpscaleNeeded = false;
	if (m_iMaxineScale == MAXINE_SCALE_MatchOutput
			|| !GetMaxineVSRTargetSize(m_videoRect, maxineTargetSize, maxineUpscaleNeeded)) {
		UpdateTexures();
	}
}
'@ @'
void CDX11VideoProcessor::SetVideoRect(const CRect& videoRect)
{
	m_videoRect = videoRect;
	UpdateRenderRect();

	// Match-output Maxine used to destroy/reload the effect on every Video Frame
	// Inc/Dec step. Keep the current inference target while input is active and
	// let the ordinary final scaler follow the presentation rectangle instantly.
	if (m_iMaxineOperation == MAXINE_OPERATION_Upscale
			&& m_iMaxineScale == MAXINE_SCALE_MatchOutput
			&& m_bMaxineVSRUsed && m_MaxineVSRSize.cx > 0 && m_MaxineVSRSize.cy > 0) {
		m_bMaxineResizePending = true;
		m_MaxineResizeLastChangeTick = GetTickCount64();
		return;
	}

	m_bMaxineResizePending = false;
	m_MaxineResizeLastChangeTick = 0;

	CSize maxineTargetSize;
	bool maxineUpscaleNeeded = false;
	if (m_iMaxineScale == MAXINE_SCALE_MatchOutput
			|| !GetMaxineVSRTargetSize(m_videoRect, maxineTargetSize, maxineUpscaleNeeded)) {
		UpdateTexures();
	}
}
'@

Write-Host 'Applied Maxine Match Output resize debounce (250 ms).'
