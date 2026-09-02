#include "stdafx.h"
#include "NvidiaOpticalFlowCapture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

namespace {

struct CpuTexture
{
	UINT width = 0;
	UINT height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT bytesPerPixel = 0;
	std::vector<uint8_t> bytes;
};

std::filesystem::path TempDirectory()
{
	std::array<wchar_t, MAX_PATH + 1> buffer = {};
	const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
	if (!length || length >= buffer.size()) {
		return std::filesystem::temp_directory_path();
	}
	return std::filesystem::path(buffer.data());
}

std::filesystem::path CaptureRequestPath()
{
	return TempDirectory() / L"MPCVR-NVOF-CAPTURE.REQUEST";
}

UINT BytesPerPixel(const DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_UINT:
		return 4;
	case DXGI_FORMAT_R8_UINT:
		return 1;
	default:
		return 0;
	}
}

const wchar_t* FormatName(const DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_B8G8R8A8_UNORM: return L"B8G8R8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return L"R8G8B8A8_UNORM";
	case DXGI_FORMAT_R16G16_SINT: return L"R16G16_SINT";
	case DXGI_FORMAT_R8_UINT: return L"R8_UINT";
	case DXGI_FORMAT_R32_UINT: return L"R32_UINT";
	default: return L"UNKNOWN";
	}
}

bool ReadTexture(
	ID3D11Device* device,
	ID3D11DeviceContext* context,
	ID3D11Texture2D* texture,
	CpuTexture& output,
	std::wstring& error)
{
	if (!device || !context || !texture) {
		error = L"A required D3D11 capture resource is null.";
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	const UINT bytesPerPixel = BytesPerPixel(desc.Format);
	if (!bytesPerPixel || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) {
		error = std::format(
			L"Unsupported capture texture format/layout: {} ({})",
			FormatName(desc.Format), static_cast<unsigned>(desc.Format));
		return false;
	}

	D3D11_TEXTURE2D_DESC stagingDesc = desc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.MiscFlags = 0;

	CComPtr<ID3D11Texture2D> staging;
	HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
	if (FAILED(hr)) {
		error = std::format(L"CreateTexture2D(capture staging) failed: 0x{:08X}", static_cast<unsigned>(hr));
		return false;
	}

	context->CopyResource(staging, texture);
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		error = std::format(L"Map(capture staging) failed: 0x{:08X}", static_cast<unsigned>(hr));
		return false;
	}

	output.width = desc.Width;
	output.height = desc.Height;
	output.format = desc.Format;
	output.bytesPerPixel = bytesPerPixel;
	const size_t tightRowBytes = static_cast<size_t>(desc.Width) * bytesPerPixel;
	output.bytes.resize(tightRowBytes * desc.Height);
	for (UINT y = 0; y < desc.Height; ++y) {
		memcpy(
			output.bytes.data() + static_cast<size_t>(y) * tightRowBytes,
			static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
			tightRowBytes);
	}
	context->Unmap(staging, 0);
	return true;
}

bool WriteBinary(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
	std::ofstream stream(path, std::ios::binary);
	if (!stream) return false;
	stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return stream.good();
}

bool WriteBmp32(const std::filesystem::path& path, const CpuTexture& image)
{
	if (image.width == 0 || image.height == 0 || image.bytesPerPixel != 4 ||
			(image.format != DXGI_FORMAT_B8G8R8A8_UNORM && image.format != DXGI_FORMAT_R8G8B8A8_UNORM)) {
		return false;
	}

	const size_t rowBytes = static_cast<size_t>(image.width) * 4;
	std::vector<uint8_t> bgra(image.bytes.size());
	if (image.format == DXGI_FORMAT_B8G8R8A8_UNORM) {
		bgra = image.bytes;
	} else {
		for (size_t i = 0; i < image.bytes.size(); i += 4) {
			bgra[i + 0] = image.bytes[i + 2];
			bgra[i + 1] = image.bytes[i + 1];
			bgra[i + 2] = image.bytes[i + 0];
			bgra[i + 3] = image.bytes[i + 3];
		}
	}

	BITMAPFILEHEADER fileHeader = {};
	BITMAPINFOHEADER infoHeader = {};
	fileHeader.bfType = 0x4D42;
	fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
	fileHeader.bfSize = static_cast<DWORD>(fileHeader.bfOffBits + bgra.size());
	infoHeader.biSize = sizeof(infoHeader);
	infoHeader.biWidth = static_cast<LONG>(image.width);
	infoHeader.biHeight = -static_cast<LONG>(image.height);
	infoHeader.biPlanes = 1;
	infoHeader.biBitCount = 32;
	infoHeader.biCompression = BI_RGB;
	infoHeader.biSizeImage = static_cast<DWORD>(rowBytes * image.height);

	std::ofstream stream(path, std::ios::binary);
	if (!stream) return false;
	stream.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
	stream.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
	stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
	return stream.good();
}

CpuTexture MakeFlowVisualization(const CpuTexture& flow)
{
	CpuTexture result;
	result.width = flow.width;
	result.height = flow.height;
	result.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	result.bytesPerPixel = 4;
	result.bytes.resize(static_cast<size_t>(flow.width) * flow.height * 4);

	const auto* vectors = reinterpret_cast<const int16_t*>(flow.bytes.data());
	const size_t count = static_cast<size_t>(flow.width) * flow.height;
	for (size_t i = 0; i < count; ++i) {
		const float x = static_cast<float>(vectors[i * 2 + 0]) / 32.0f;
		const float y = static_cast<float>(vectors[i * 2 + 1]) / 32.0f;
		const auto encode = [](const float value) {
			return static_cast<uint8_t>(std::clamp(128.0f + value * 4.0f, 0.0f, 255.0f));
		};
		result.bytes[i * 4 + 0] = 128;
		result.bytes[i * 4 + 1] = encode(y);
		result.bytes[i * 4 + 2] = encode(x);
		result.bytes[i * 4 + 3] = 255;
	}
	return result;
}

CpuTexture MakeCostVisualization(const CpuTexture& cost)
{
	CpuTexture result;
	result.width = cost.width;
	result.height = cost.height;
	result.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	result.bytesPerPixel = 4;
	result.bytes.resize(static_cast<size_t>(cost.width) * cost.height * 4);

	const size_t count = static_cast<size_t>(cost.width) * cost.height;
	for (size_t i = 0; i < count; ++i) {
		uint8_t value = 0;
		if (cost.format == DXGI_FORMAT_R8_UINT) {
			value = cost.bytes[i];
		} else if (cost.format == DXGI_FORMAT_R32_UINT) {
			const uint32_t raw = reinterpret_cast<const uint32_t*>(cost.bytes.data())[i];
			value = static_cast<uint8_t>(std::min<uint32_t>(raw, 255));
		}
		result.bytes[i * 4 + 0] = value;
		result.bytes[i * 4 + 1] = value;
		result.bytes[i * 4 + 2] = value;
		result.bytes[i * 4 + 3] = 255;
	}
	return result;
}

CpuTexture MakeConsistencyVisualization(const CpuTexture& forward, const CpuTexture& backward)
{
	CpuTexture result;
	result.width = forward.width;
	result.height = forward.height;
	result.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	result.bytesPerPixel = 4;
	result.bytes.resize(static_cast<size_t>(result.width) * result.height * 4);

	const auto* f = reinterpret_cast<const int16_t*>(forward.bytes.data());
	const auto* b = reinterpret_cast<const int16_t*>(backward.bytes.data());
	for (UINT y = 0; y < result.height; ++y) {
		for (UINT x = 0; x < result.width; ++x) {
			const size_t index = static_cast<size_t>(y) * result.width + x;
			const float fx = static_cast<float>(f[index * 2 + 0]) / 32.0f;
			const float fy = static_cast<float>(f[index * 2 + 1]) / 32.0f;
			const float sourceX = (static_cast<float>(x) + 0.5f) * 4.0f - 0.5f + fx;
			const float sourceY = (static_cast<float>(y) + 0.5f) * 4.0f - 0.5f + fy;
			const int bx = std::clamp(static_cast<int>(std::lround(sourceX / 4.0f)), 0, static_cast<int>(result.width) - 1);
			const int by = std::clamp(static_cast<int>(std::lround(sourceY / 4.0f)), 0, static_cast<int>(result.height) - 1);
			const size_t backwardIndex = static_cast<size_t>(by) * result.width + static_cast<UINT>(bx);
			const float bxv = static_cast<float>(b[backwardIndex * 2 + 0]) / 32.0f;
			const float byv = static_cast<float>(b[backwardIndex * 2 + 1]) / 32.0f;
			const float error = std::sqrt((fx + bxv) * (fx + bxv) + (fy + byv) * (fy + byv));
			const uint8_t value = static_cast<uint8_t>(std::clamp(error * 24.0f, 0.0f, 255.0f));
			result.bytes[index * 4 + 0] = value;
			result.bytes[index * 4 + 1] = value;
			result.bytes[index * 4 + 2] = value;
			result.bytes[index * 4 + 3] = 255;
		}
	}
	return result;
}

std::filesystem::path MakeCaptureDirectory()
{
	SYSTEMTIME time = {};
	GetLocalTime(&time);
	const auto root = TempDirectory() / L"MPCVR-NVOF-Captures";
	std::filesystem::create_directories(root);
	const std::wstring name = std::format(
		L"capture-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-pid{}",
		time.wYear, time.wMonth, time.wDay,
		time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
		GetCurrentProcessId());
	const auto directory = root / name;
	std::filesystem::create_directories(directory);
	return directory;
}

} // namespace

bool IsNativeNvofCaptureRequested()
{
	const auto path = CaptureRequestPath();
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool CaptureNativeNvofFramePair(
	const NativeNvofCaptureInputs& inputs,
	std::wstring& outputDirectory,
	std::wstring& errorMessage)
{
	outputDirectory.clear();
	errorMessage.clear();
	DeleteFileW(CaptureRequestPath().c_str());

	if (!inputs.device || !inputs.context || !inputs.firstFrame || !inputs.secondFrame ||
			!inputs.midpointFrame || !inputs.forwardFlow || !inputs.backwardFlow) {
		errorMessage = L"Capture request reached an incomplete native NVOF frame pair.";
		return false;
	}

	try {
		const auto directory = MakeCaptureDirectory();
		CpuTexture first;
		CpuTexture second;
		CpuTexture midpoint;
		CpuTexture forward;
		CpuTexture backward;
		CpuTexture forwardCost;
		CpuTexture backwardCost;

		if (!ReadTexture(inputs.device, inputs.context, inputs.firstFrame, first, errorMessage) ||
				!ReadTexture(inputs.device, inputs.context, inputs.secondFrame, second, errorMessage) ||
				!ReadTexture(inputs.device, inputs.context, inputs.midpointFrame, midpoint, errorMessage) ||
				!ReadTexture(inputs.device, inputs.context, inputs.forwardFlow, forward, errorMessage) ||
				!ReadTexture(inputs.device, inputs.context, inputs.backwardFlow, backward, errorMessage)) {
			return false;
		}

		if (!WriteBmp32(directory / L"frame-A.bmp", first) ||
				!WriteBmp32(directory / L"frame-B.bmp", second) ||
				!WriteBmp32(directory / L"midpoint-current.bmp", midpoint) ||
				!WriteBinary(directory / L"flow-forward-B-to-A-s10.5.bin", forward.bytes) ||
				!WriteBinary(directory / L"flow-backward-A-to-B-s10.5.bin", backward.bytes) ||
				!WriteBmp32(directory / L"flow-forward-B-to-A.bmp", MakeFlowVisualization(forward)) ||
				!WriteBmp32(directory / L"flow-backward-A-to-B.bmp", MakeFlowVisualization(backward)) ||
				!WriteBmp32(directory / L"flow-consistency.bmp", MakeConsistencyVisualization(forward, backward))) {
			errorMessage = L"Could not write one or more native NVOF diagnostic files.";
			return false;
		}

		bool haveCost = false;
		if (inputs.forwardCost && inputs.backwardCost) {
			std::wstring costError;
			if (ReadTexture(inputs.device, inputs.context, inputs.forwardCost, forwardCost, costError) &&
					ReadTexture(inputs.device, inputs.context, inputs.backwardCost, backwardCost, costError)) {
				haveCost = WriteBinary(directory / L"cost-forward.bin", forwardCost.bytes) &&
					WriteBinary(directory / L"cost-backward.bin", backwardCost.bytes) &&
					WriteBmp32(directory / L"cost-forward.bmp", MakeCostVisualization(forwardCost)) &&
					WriteBmp32(directory / L"cost-backward.bmp", MakeCostVisualization(backwardCost));
			}
		}

		std::wofstream metadata(directory / L"metadata.txt");
		if (!metadata) {
			errorMessage = L"Could not create capture metadata.txt.";
			return false;
		}
		metadata << L"MPCVR Native NVOF real-frame capture v1\n";
		metadata << L"frame_width=" << inputs.frameWidth << L"\n";
		metadata << L"frame_height=" << inputs.frameHeight << L"\n";
		metadata << L"flow_width=" << inputs.flowWidth << L"\n";
		metadata << L"flow_height=" << inputs.flowHeight << L"\n";
		metadata << L"grid_size=4\n";
		metadata << L"midpoint_time=" << inputs.midpointTime << L"\n";
		metadata << L"timestamp_A=" << inputs.firstTimestamp << L"\n";
		metadata << L"timestamp_B=" << inputs.secondTimestamp << L"\n";
		metadata << L"frame_A_format=" << FormatName(first.format) << L"\n";
		metadata << L"frame_B_format=" << FormatName(second.format) << L"\n";
		metadata << L"midpoint_format=" << FormatName(midpoint.format) << L"\n";
		metadata << L"flow_format=R16G16_SINT\n";
		metadata << L"flow_encoding=S10.5 little-endian int16 x,y; divide by 32 for pixels\n";
		metadata << L"forward_direction=B_to_A\n";
		metadata << L"backward_direction=A_to_B\n";
		metadata << L"hardware_cost_present=" << (haveCost ? 1 : 0) << L"\n";
		if (haveCost) {
			metadata << L"hardware_cost_format=" << FormatName(forwardCost.format) << L"\n";
		}
		metadata.flush();

		std::ofstream marker(directory / L"CAPTURE-COMPLETE.txt");
		marker << "Capture complete. Zip this whole directory and attach it to the ChatGPT thread.\n";
		outputDirectory = directory.wstring();
		return true;
	} catch (const std::exception&) {
		errorMessage = L"An exception occurred while writing native NVOF diagnostics.";
		return false;
	}
}
