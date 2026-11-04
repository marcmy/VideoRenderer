/*
 * (C) 2018-2026 see Authors.txt
 *
 * Settings dialog for the renderer hotkeys (Maxine, RIFE, SDR enhancement).
 */

#pragma once

#include "IVideoRenderer.h"

bool ShowFeatureHotkeySettings(HWND parent, HINSTANCE instance, Settings_t& settings);

// True while the hotkey settings are open: the renderer's hotkeys are paused
// so that their key boxes can capture any combination.
bool FeatureHotkeyCaptureActive();
