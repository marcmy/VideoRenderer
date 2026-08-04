/*
 * (C) 2018-2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <dxva2api.h>

enum :int {
	TEXFMT_AUTOINT = 0,
	TEXFMT_8INT = 8,
	TEXFMT_10INT = 10,
	TEXFMT_16FLOAT = 16,
};

enum :int {
	DEINT_Disable = 0,
	DEINT_Enable = 1,
	DEINT_HackFutureFrames = 2,
};

enum :int {
	SUPERRES_Disable = 0,
	SUPERRES_SD,
	SUPERRES_720p,
	SUPERRES_1080p,
	SUPERRES_1440p,
	SUPERRES_COUNT
};

enum :int {
	MAXINE_OPERATION_Disabled = 0,
	MAXINE_OPERATION_Upscale,
	MAXINE_OPERATION_Denoise,
	MAXINE_OPERATION_Deblur,
	MAXINE_OPERATION_COUNT
};

enum :int {
	MAXINE_SOURCE_Auto = 0,
	MAXINE_SOURCE_Standard,
	MAXINE_SOURCE_HighBitrate,
	MAXINE_SOURCE_Bicubic,
	MAXINE_SOURCE_COUNT
};

enum :int {
	MAXINE_QUALITY_Low = 1,
	MAXINE_QUALITY_Medium,
	MAXINE_QUALITY_High,
	MAXINE_QUALITY_Ultra,
};

enum :int {
	MAXINE_SCALE_MatchOutput = 0,
	MAXINE_SCALE_4_3X = 133,
	MAXINE_SCALE_1_5X = 150,
	MAXINE_SCALE_2X = 200,
	MAXINE_SCALE_3X = 300,
	MAXINE_SCALE_4X = 400,
};

enum :int {
	MAXINE_OVERSAMPLE_Off = 100,
	MAXINE_OVERSAMPLE_4_3X = 133,
	MAXINE_OVERSAMPLE_1_5X = 150,
	MAXINE_OVERSAMPLE_2X = 200,
};

enum :int {
	MAXINE_FILTER_Off = 0,
	MAXINE_FILTER_Low,
	MAXINE_FILTER_Medium,
	MAXINE_FILTER_High,
	MAXINE_FILTER_Ultra,
	MAXINE_FILTER_COUNT
};

enum :int {
	MAXINE_PIPELINE_UpscaleDenoiseDeblur = 0,
	MAXINE_PIPELINE_UpscaleDeblurDenoise,
	MAXINE_PIPELINE_DenoiseDeblurUpscale,
	MAXINE_PIPELINE_DeblurDenoiseUpscale,
	MAXINE_PIPELINE_DenoiseUpscaleDeblur,
	MAXINE_PIPELINE_DeblurUpscaleDenoise,
	MAXINE_PIPELINE_COUNT
};

constexpr inline int MAXINE_GPU_Auto = -1;
constexpr inline int MAXINE_AUTO_BITRATE_DEF = 20;
constexpr inline int MAXINE_AUTO_BITRATE_MIN = 1;
constexpr inline int MAXINE_AUTO_BITRATE_MAX = 1000;


enum :int {
	FRUC_MODE_Disabled = 0,
	FRUC_MODE_Double,
	FRUC_MODE_COUNT
};

enum :int {
	FRUC_SOURCE_LIMIT_720p = 0,
	FRUC_SOURCE_LIMIT_1080p,
	FRUC_SOURCE_LIMIT_1440p,
	FRUC_SOURCE_LIMIT_2160p,
	FRUC_SOURCE_LIMIT_COUNT
};

enum :int {
	FRUC_MAX_OUTPUT_60 = 60,
	FRUC_MAX_OUTPUT_120 = 120,
	FRUC_MAX_OUTPUT_240 = 240,
};

constexpr inline int FRUC_GPU_Auto = -1;

enum :int {
	CHROMA_Nearest = 0,
	CHROMA_Bilinear,
	CHROMA_CatmullRom,
	CHROMA_COUNT
};

enum :int {
	UPSCALE_Nearest = 0,
	UPSCALE_Mitchell,
	UPSCALE_CatmullRom,
	UPSCALE_Lanczos2,
	UPSCALE_Lanczos3,
	UPSCALE_Jinc2,
	UPSCALE_COUNT
};

enum :int {
	DOWNSCALE_Box = 0,
	DOWNSCALE_Bilinear,
	DOWNSCALE_Hamming,
	DOWNSCALE_Bicubic,
	DOWNSCALE_BicubicSharp,
	DOWNSCALE_Lanczos,
	DOWNSCALE_COUNT
};

enum :int {
	SWAPEFFECT_Discard = 0,
	SWAPEFFECT_Flip,
	SWAPEFFECT_COUNT
};

enum :int {
	HDRTD_Disabled = 0,
	HDRTD_On_Fullscreen,
	HDRTD_On,
	HDRTD_OnOff_Fullscreen,
	HDRTD_OnOff
};

#define SDR_NITS_DEF 125
#define SDR_NITS_MIN  25
#define SDR_NITS_MAX 400
#define SDR_NITS_STEP  5

constexpr inline auto HDR_NITS_DEF = 1000;
constexpr inline auto HDR_NITS_MIN = 100;
constexpr inline auto HDR_NITS_MAX = 10000;

struct VPEnableFormats_t {
	bool bNV12;
	bool bP01x;
	bool bYUY2;
	bool bOther;
};

struct Settings_t {
	bool bUseD3D11;
	bool bShowStats;
	int  iResizeStats;
	int  iTexFormat;
	VPEnableFormats_t VPFmts;
	int  iVPDeinterlacing;
	bool bDeintDouble;
	bool bVPScaling;
	int iVPSuperRes;
	bool bVPRTXVideoHDR;
	int  iChromaScaling;
	int  iUpscaling;
	int  iDownscaling;
	bool bInterpolateAt50pct;
	bool bUseDither;
	bool bDeintBlend;
	int  iSwapEffect;
	bool bExclusiveFS;
	bool bVBlankBeforePresent;
	bool bAdjustPresentTime;
	bool bReinitByDisplay;
	bool bHdrPreferDoVi;
	bool bHdrPassthrough;
	int  iHdrToggleDisplay;
	int  iHdrOsdBrightness;
	bool bConvertToSdr;
	int  iSDRDisplayNits;
	bool bHdrLocalToneMapping;
	int  iHdrLocalToneMappingType;
	int iHdrDisplayMaxNits;
	int iMaxineOperation;
	int iMaxineSourceMode;
	int iMaxineQuality;
	int iMaxineScale;
	int iMaxineOversample;
	int iMaxineSourceLimit;
	int iMaxineDenoise;
	int iMaxineDeblur;
	int iMaxinePipeline;
	int iMaxineGPU;
	int iMaxineAutoBitrate;
	int iFrameInterpolationMode;
	int iFrameInterpolationSourceLimit;
	int iFrameInterpolationMaxOutput;
	int iFrameInterpolationGPU;
	bool bFrameInterpolationFallback;

	Settings_t() {
		SetDefault();
	}

	void SetDefault() {
		if (IsWindows8OrGreater()) {
			bUseD3D11                   = true;
		} else {
			bUseD3D11                   = false;
		}
		bShowStats                      = false;
		iResizeStats                    = 0;
		iTexFormat                      = TEXFMT_AUTOINT;
		VPFmts.bNV12                    = true;
		VPFmts.bP01x                    = true;
		VPFmts.bYUY2                    = true;
		VPFmts.bOther                   = true;
		iVPDeinterlacing                = DEINT_Enable;
		bDeintDouble                    = true;
		bVPScaling                      = true;
		iVPSuperRes                     = SUPERRES_Disable;
		bVPRTXVideoHDR                  = false;
		iChromaScaling                  = CHROMA_Bilinear;
		iUpscaling                      = UPSCALE_CatmullRom;
		iDownscaling                    = DOWNSCALE_Hamming;
		bInterpolateAt50pct             = true;
		bUseDither                      = true;
		bDeintBlend                     = false;
		iSwapEffect                     = SWAPEFFECT_Flip;
		bExclusiveFS                    = false;
		bVBlankBeforePresent            = false;
		bAdjustPresentTime              = true;
		bReinitByDisplay                = false;
		bHdrPreferDoVi                  = false;
		if (IsWindows10OrGreater()) {
			bHdrPassthrough             = true;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		} else {
			bHdrPassthrough             = false;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			iHdrDisplayMaxNits          = 1000;
		}
		iHdrToggleDisplay               = HDRTD_Disabled;
		bConvertToSdr                   = true;
		iHdrOsdBrightness               = 0;
		iSDRDisplayNits                 = SDR_NITS_DEF;
		iMaxineOperation                = MAXINE_OPERATION_Disabled;
		iMaxineSourceMode               = MAXINE_SOURCE_Auto;
		iMaxineQuality                  = MAXINE_QUALITY_High;
		iMaxineScale                    = MAXINE_SCALE_MatchOutput;
		iMaxineOversample               = MAXINE_OVERSAMPLE_Off;
		iMaxineSourceLimit              = SUPERRES_1080p;
		iMaxineDenoise                  = MAXINE_FILTER_Off;
		iMaxineDeblur                   = MAXINE_FILTER_Off;
		iMaxinePipeline                 = MAXINE_PIPELINE_UpscaleDenoiseDeblur;
		iMaxineGPU                      = MAXINE_GPU_Auto;
		iMaxineAutoBitrate              = MAXINE_AUTO_BITRATE_DEF;
		iFrameInterpolationMode         = FRUC_MODE_Disabled;
		iFrameInterpolationSourceLimit  = FRUC_SOURCE_LIMIT_1080p;
		iFrameInterpolationMaxOutput    = FRUC_MAX_OUTPUT_60;
		iFrameInterpolationGPU          = FRUC_GPU_Auto;
		bFrameInterpolationFallback     = true;
	}
};

interface __declspec(uuid("1AB00F10-5F55-42AC-B53F-38649F11BE3E"))
IVideoRenderer : public IUnknown {
	STDMETHOD(GetVideoProcessorInfo) (std::wstring& str) PURE;
	STDMETHOD_(bool, GetActive()) PURE;

	STDMETHOD_(void, GetSettings(Settings_t& setings)) PURE;
	STDMETHOD_(void, SetSettings(const Settings_t& setings)) PURE;

	STDMETHOD(SaveSettings()) PURE;
};
