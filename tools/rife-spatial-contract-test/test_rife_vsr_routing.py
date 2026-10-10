from pathlib import Path

repo = Path(__file__).resolve().parents[2]
read = lambda file: (repo / 'Source' / file).read_text(encoding='utf-8-sig')
dx = read('DX11VideoProcessor.cpp')
bridge = read('RifeDX11Bridge.cpp')
vp = read('D3D11VP.cpp')
vp_header = read('D3D11VP.h')
maxine = read('NvidiaMaxineVSR.cpp')

start = dx.index('HRESULT CDX11VideoProcessor::Process(')
end = dx.index('\nvoid CDX11VideoProcessor::SetVideoRect', start)
process = dx[start:end]
native = process.index('TryRifeVideoProcessorUpscale(')
assert process.index('cudaOutput->Export(', process.index('if (cudaOutput && !enhanced)')) < native
assert native < process.index('return ResizeShaderPass(*pInputTexture', native)
assert 'if (!rifeSourcePreparation)' in process[:process.index('if (m_pFrameInterpolationTexture')]

native = bridge[bridge.index('bool CDX11VideoProcessor::TryRifeVideoProcessorUpscale('):bridge.index('DXGI_FORMAT CDX11VideoProcessor::GetRifeSurfaceFormat')]
assert 'm_iMaxineOperation != MAXINE_OPERATION_Disabled' in native
assert 'input.desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM' in native
assert '!SourceIsHDR() && !m_bVPUseRTXVideoHDR' in native
assert 'SetRectangles(driverRect, vpRect)' in native
assert 'SetSuperRes(m_iVPSuperRes, driverRect.Size())' in native
conversion = native.index('m_RifeVsrConvertVP.Process(m_TexRifeVsrNV12.pTexture')
rectangles = native.index('m_RifeUpscaleVP.SetRectangles(driverRect, vpRect)', conversion)
request = native.index('m_RifeUpscaleVP.SetSuperRes(m_iVPSuperRes, driverRect.Size())', rectangles)
blit = native.index('m_RifeUpscaleVP.Process(vpTarget', request)
assert conversion < rectangles < request < blit
assert 'if (hr != S_OK)' in native[request:blit]
assert 'ResolveRifeVsrContentSize(contentSize,' in native
assert '{m_srcRectWidth, m_srcRectHeight}, m_srcAnamorphic, m_iRotation)' in native
assert 'restoreEncodedGrid || probeMode ? contentRect : fullInput' in native
assert 'restoreEncodedGrid || probeMode ? driverRect : fullInput' in native
assert 'InitInputTextures(m_pDevice, false)' in native
assert 'm_RifeVsrConvertVP.Process(m_TexRifeVsrNV12.pTexture' in native
assert 'm_RifeUpscaleVP.SetInputVideoData(m_TexRifeVsrNV12.pTexture' in native
assert 'nv12Color.NominalRange = DXVA2_NominalRange_16_235' in native
assert 'nv12Color.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT709' in native
assert 'nv12Color.VideoTransferFunction = DXVA2_VideoTransFunc_22' in native
assert 'Tex2D_DefaultRTarget' in native
assert 'activity unverified' in native
assert 'SuperResolution*' not in dx
assert 'if (nv12Output)' in vp
assert 'DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709' in vp
assert 'if (FAILED(hr) && !nv12Output)' in vp
assert 'input.desc.Width != m_RifeUpscaleInputDesc.Width' in native
assert 'contentRect != m_RifeUpscaleContentRect' in native
assert 'driverSize != m_RifeVsrContentSize' in native
assert 'dstRect.Size() != m_RifeUpscaleOutputSize' in native
assert 'm_D3D11VP.' not in native, 'Presentation must not mutate decoder/field VP history'
assert 'SetProcAmpValues' not in native and 'SetRotation' not in native
assert 'CanUseRifeVideoProcessorUpscale' in native
assert 'ReleaseVideoDevice' in vp
input_data = vp_header[vp_header.index('class VideoInputData'):vp_header.index('// D3D11 Video Processor')]
clear = input_data[input_data.index('void Clear()'):input_data.index('void ClearInputDecoderViews()')]
assert 'm_InputDecoderViews.clear()' in clear and 'm_Texture.Release()' in clear
assert 'm_RifeUpscaleVP.ReleaseVideoDevice()' in dx
assert 'm_RifeVsrConvertVP.ReleaseVideoDevice()' in dx
assert 'm_RifeVsrConvertVP.ReleaseVideoProcessor()' in dx
assert 'm_TexRifeVsrNV12.Release()' in dx
assert 'm_RifeUpscaleVP.ReleaseVideoProcessor()' in dx
assert 'm_TexRifeUpscaleOutput.Release()' in dx
assert 'ResolveMaxineUpscaleEnvelope' in dx
assert 'runFailure.HasDifferentBinding(runBinding)' in maxine
assert 'code == -7 && std::wstring_view(failedOperation) == L"NvVFX_Run"' in maxine
assert 'runFailure.Record(runBinding)' in maxine
print('RIFE / native VSR / Maxine routing contracts passed (CPU source checks)')
