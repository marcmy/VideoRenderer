#pragma once

// Temporary migration wrapper: keep the existing renderer declaration intact
// while adding explicit legacy implementations for RIFE settings persistence.
// This file can collapse back into VideoRenderer.h after the RIFE path replaces
// the old NvOFFRUC implementation end-to-end.
#include "IVideoRenderer.h"

#define GetSettings(x) GetSettingsLegacy(x); STDMETHODIMP_(void) GetSettings(x)
#define SetSettings(x) SetSettingsLegacy(x); STDMETHODIMP_(void) SetSettings(x)
#define SaveSettings() SaveSettingsLegacy(); STDMETHODIMP SaveSettings()
#include "VideoRendererLegacy.h"
#undef SaveSettings
#undef SetSettings
#undef GetSettings
