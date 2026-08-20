/*
 * Process-local shared-memory telemetry for MPCVR Unified Setup calibration.
 *
 * The block contains only renderer state and timing counters. It intentionally
 * excludes media filenames, paths, titles, and other user content.
 */

#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

constexpr uint32_t MPCVR_CALIBRATION_TELEMETRY_MAGIC = 0x5256504d; // "MPVR"
constexpr uint32_t MPCVR_CALIBRATION_TELEMETRY_VERSION = 1;

namespace MpcvrCalibrationTelemetryFlags {
	constexpr uint32_t RendererActive = 1u << 0;
	constexpr uint32_t MaxineEnabled = 1u << 1;
	constexpr uint32_t MaxineActive = 1u << 2;
	constexpr uint32_t FrameInterpolationEnabled = 1u << 3;
	constexpr uint32_t FrameInterpolationActive = 1u << 4;
	constexpr uint32_t CombinedActive = 1u << 5;
}

struct MpcvrCalibrationTelemetryData
{
	uint32_t magic = MPCVR_CALIBRATION_TELEMETRY_MAGIC;
	uint32_t version = MPCVR_CALIBRATION_TELEMETRY_VERSION;
	uint32_t structSize = sizeof(MpcvrCalibrationTelemetryData);
	volatile LONG sequence = 0;
	uint32_t processId = 0;
	uint32_t flags = 0;
	uint64_t updatedTickMilliseconds = 0;
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint64_t frames = 0;
	uint32_t droppedFrames = 0;
	uint32_t skippedFrames = 0;
	uint32_t failedFrames = 0;
	uint32_t reserved0 = 0;
	double sourceFps = 0.0;
	double targetOutputFps = 0.0;
	double measuredDrawFps = 0.0;
	double maxineVsrMilliseconds = 0.0;
	double maxineDenoiseMilliseconds = 0.0;
	double maxineDeblurMilliseconds = 0.0;
	double maxineTotalMilliseconds = 0.0;
	double frameInterpolationMilliseconds = 0.0;
	double combinedProcessingMilliseconds = 0.0;
	double sourceFrameBudgetMilliseconds = 0.0;
	double timingHeadroomMilliseconds = 0.0;
	int32_t maxineOperation = 0;
	int32_t maxineQuality = 0;
	int32_t maxineScale = 0;
	int32_t maxineOversample = 0;
	int32_t frameInterpolationMode = 0;
	int32_t frameInterpolationSourceLimit = 0;
	int32_t frameInterpolationMaxOutput = 0;
	int32_t reserved1 = 0;
};

static_assert(offsetof(MpcvrCalibrationTelemetryData, sequence) == 12);
static_assert(offsetof(MpcvrCalibrationTelemetryData, updatedTickMilliseconds) == 24);
static_assert(offsetof(MpcvrCalibrationTelemetryData, sourceFps) == 72);
static_assert(sizeof(MpcvrCalibrationTelemetryData) == 192);

struct MpcvrCalibrationTelemetrySnapshot
{
	uint32_t flags = 0;
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint64_t frames = 0;
	uint32_t droppedFrames = 0;
	uint32_t skippedFrames = 0;
	uint32_t failedFrames = 0;
	double sourceFps = 0.0;
	double targetOutputFps = 0.0;
	double measuredDrawFps = 0.0;
	double maxineVsrMilliseconds = 0.0;
	double maxineDenoiseMilliseconds = 0.0;
	double maxineDeblurMilliseconds = 0.0;
	double maxineTotalMilliseconds = 0.0;
	double frameInterpolationMilliseconds = 0.0;
	double combinedProcessingMilliseconds = 0.0;
	double sourceFrameBudgetMilliseconds = 0.0;
	double timingHeadroomMilliseconds = 0.0;
	int32_t maxineOperation = 0;
	int32_t maxineQuality = 0;
	int32_t maxineScale = 0;
	int32_t maxineOversample = 0;
	int32_t frameInterpolationMode = 0;
	int32_t frameInterpolationSourceLimit = 0;
	int32_t frameInterpolationMaxOutput = 0;
};

class CMpcvrCalibrationTelemetry final
{
public:
	CMpcvrCalibrationTelemetry()
	{
		wchar_t mappingName[128] = {};
		swprintf_s(mappingName, L"Local\\MPCVR.UnifiedSetup.Telemetry.%lu", GetCurrentProcessId());

		m_mapping = CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			nullptr,
			PAGE_READWRITE,
			0,
			static_cast<DWORD>(sizeof(MpcvrCalibrationTelemetryData)),
			mappingName);
		if (!m_mapping) {
			return;
		}

		m_data = static_cast<MpcvrCalibrationTelemetryData*>(MapViewOfFile(
			m_mapping,
			FILE_MAP_ALL_ACCESS,
			0,
			0,
			sizeof(MpcvrCalibrationTelemetryData)));
		if (!m_data) {
			CloseHandle(m_mapping);
			m_mapping = nullptr;
			return;
		}

		ZeroMemory(m_data, sizeof(*m_data));
		m_data->magic = MPCVR_CALIBRATION_TELEMETRY_MAGIC;
		m_data->version = MPCVR_CALIBRATION_TELEMETRY_VERSION;
		m_data->structSize = sizeof(MpcvrCalibrationTelemetryData);
		m_data->processId = GetCurrentProcessId();
		m_data->updatedTickMilliseconds = GetTickCount64();
	}

	~CMpcvrCalibrationTelemetry()
	{
		if (m_data) {
			BeginWrite();
			m_data->flags = 0;
			m_data->updatedTickMilliseconds = GetTickCount64();
			EndWrite();
			UnmapViewOfFile(m_data);
			m_data = nullptr;
		}
		if (m_mapping) {
			CloseHandle(m_mapping);
			m_mapping = nullptr;
		}
	}

	CMpcvrCalibrationTelemetry(const CMpcvrCalibrationTelemetry&) = delete;
	CMpcvrCalibrationTelemetry& operator=(const CMpcvrCalibrationTelemetry&) = delete;

	void Publish(const MpcvrCalibrationTelemetrySnapshot& snapshot)
	{
		if (!m_data) {
			return;
		}

		BeginWrite();
		m_data->processId = GetCurrentProcessId();
		m_data->flags = snapshot.flags;
		m_data->updatedTickMilliseconds = GetTickCount64();
		m_data->sourceWidth = snapshot.sourceWidth;
		m_data->sourceHeight = snapshot.sourceHeight;
		m_data->outputWidth = snapshot.outputWidth;
		m_data->outputHeight = snapshot.outputHeight;
		m_data->frames = snapshot.frames;
		m_data->droppedFrames = snapshot.droppedFrames;
		m_data->skippedFrames = snapshot.skippedFrames;
		m_data->failedFrames = snapshot.failedFrames;
		m_data->sourceFps = snapshot.sourceFps;
		m_data->targetOutputFps = snapshot.targetOutputFps;
		m_data->measuredDrawFps = snapshot.measuredDrawFps;
		m_data->maxineVsrMilliseconds = snapshot.maxineVsrMilliseconds;
		m_data->maxineDenoiseMilliseconds = snapshot.maxineDenoiseMilliseconds;
		m_data->maxineDeblurMilliseconds = snapshot.maxineDeblurMilliseconds;
		m_data->maxineTotalMilliseconds = snapshot.maxineTotalMilliseconds;
		m_data->frameInterpolationMilliseconds = snapshot.frameInterpolationMilliseconds;
		m_data->combinedProcessingMilliseconds = snapshot.combinedProcessingMilliseconds;
		m_data->sourceFrameBudgetMilliseconds = snapshot.sourceFrameBudgetMilliseconds;
		m_data->timingHeadroomMilliseconds = snapshot.timingHeadroomMilliseconds;
		m_data->maxineOperation = snapshot.maxineOperation;
		m_data->maxineQuality = snapshot.maxineQuality;
		m_data->maxineScale = snapshot.maxineScale;
		m_data->maxineOversample = snapshot.maxineOversample;
		m_data->frameInterpolationMode = snapshot.frameInterpolationMode;
		m_data->frameInterpolationSourceLimit = snapshot.frameInterpolationSourceLimit;
		m_data->frameInterpolationMaxOutput = snapshot.frameInterpolationMaxOutput;
		EndWrite();
	}

private:
	void BeginWrite()
	{
		LONG sequence = InterlockedIncrement(&m_data->sequence);
		if ((sequence & 1) == 0) {
			InterlockedIncrement(&m_data->sequence);
		}
		MemoryBarrier();
	}

	void EndWrite()
	{
		MemoryBarrier();
		LONG sequence = InterlockedIncrement(&m_data->sequence);
		if ((sequence & 1) != 0) {
			InterlockedIncrement(&m_data->sequence);
		}
	}

	HANDLE m_mapping = nullptr;
	MpcvrCalibrationTelemetryData* m_data = nullptr;
};
