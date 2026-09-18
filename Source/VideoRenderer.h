#pragma once

// Temporary migration wrapper: keep the existing renderer declaration intact
// while adding explicit legacy implementations for RIFE settings/presentation.
// This file can collapse back into VideoRenderer.h after the RIFE path replaces
// the old NvOFFRUC implementation end-to-end.
#include <memory>
#include "IVideoRenderer.h"

class CRifePlaybackPipeline;

#define GetSettings(x) GetSettingsLegacy(x); STDMETHODIMP_(void) GetSettings(x)
#define SetSettings(x) SetSettingsLegacy(x); STDMETHODIMP_(void) SetSettings(x)
#define SaveSettings() SaveSettingsLegacy(); STDMETHODIMP SaveSettings()
#define Receive(x) ReceiveLegacy(x); HRESULT Receive(x)
#define StopFrameInterpolationPresenter() StopFrameInterpolationPresenter(); friend class CRifePlaybackPipeline; std::unique_ptr<CRifePlaybackPipeline> m_RifePipeline
#include "VideoRendererLegacy.h"
#undef StopFrameInterpolationPresenter
#undef Receive
#undef SaveSettings
#undef SetSettings
#undef GetSettings
