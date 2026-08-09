from pathlib import Path

path = Path('Source/NvidiaOpticalFlowNative.cpp')
source = path.read_text(encoding='utf-8')
if 'CaptureNativeNvofFramePair' in source:
    print('Real-frame capture is already wired.')
    raise SystemExit(0)


def replace_once(old: str, new: str) -> None:
    global source
    if old not in source:
        raise RuntimeError('Could not find patch target:\n' + old[:500])
    source = source.replace(old, new, 1)


replace_once(
    '#include "NvidiaOpticalFlowNative.h"\n',
    '#include "NvidiaOpticalFlowNative.h"\n#include "NvidiaOpticalFlowCapture.h"\n')

replace_once(
    '\tRegisteredSurface forwardFlow;\n\tRegisteredSurface backwardFlow;\n',
    '\tRegisteredSurface forwardFlow;\n\tRegisteredSurface backwardFlow;\n'
    '\tRegisteredSurface forwardCost;\n\tRegisteredSurface backwardCost;\n'
    '\tbool costCaptureEnabled = false;\n')

replace_once(
    '\t\tUnregister(forwardFlow);\n\t\tUnregister(backwardFlow);\n',
    '\t\tUnregister(forwardCost);\n\t\tUnregister(backwardCost);\n'
    '\t\tUnregister(forwardFlow);\n\t\tUnregister(backwardFlow);\n')

replace_once(
    '\t\truntimeInfo.clear();\n\t}\n',
    '\t\truntimeInfo.clear();\n\t\tcostCaptureEnabled = false;\n\t}\n')

marker = '\n\tbool CreateSynthesisResources()\n\t{\n'
if marker not in source:
    raise RuntimeError('Could not find CreateSynthesisResources marker.')

cost_function = '''
\tbool CreateCostSurface(RegisteredSurface& surface)
\t{
\t\tD3D11_TEXTURE2D_DESC desc = {};
\t\tdesc.Width = flowWidth;
\t\tdesc.Height = flowHeight;
\t\tdesc.MipLevels = 1;
\t\tdesc.ArraySize = 1;
\t\tdesc.Format = DXGI_FORMAT_R8_UINT;
\t\tdesc.SampleDesc.Count = 1;
\t\tdesc.Usage = D3D11_USAGE_DEFAULT;
\t\tdesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
\t\tHRESULT hr = device->CreateTexture2D(&desc, nullptr, &surface.texture);
\t\tif (FAILED(hr)) {
\t\t\tstatus = std::format(L"CreateTexture2D(native diagnostic cost) failed ({})", HR2Str(hr));
\t\t\treturn false;
\t\t}
\t\thr = device->CreateShaderResourceView(surface.texture, nullptr, &surface.view);
\t\tif (FAILED(hr)) {
\t\t\tstatus = std::format(L"CreateShaderResourceView(native diagnostic cost) failed ({})", HR2Str(hr));
\t\t\treturn false;
\t\t}
\t\tconst nvof::Status code = api.registerResourceD3D11(
\t\t\tsession, surface.texture, &surface.nvofHandle);
\t\tif (code != nvof::Success) {
\t\t\tstatus = std::format(L"NvOFRegisterResourceD3D11(diagnostic cost) failed: {}", DriverError(code));
\t\t\treturn false;
\t\t}
\t\treturn true;
\t}
'''
source = source.replace(marker, '\n' + cost_function + marker, 1)

replace_once(
    '\t\tstd::vector<DXGI_FORMAT> inputFormats;\n'
    '\t\tstd::vector<DXGI_FORMAT> outputFormats;\n'
    '\t\tif (!QueryFormats(nvof::BufferUsageInput, inputFormats) ||\n'
    '\t\t\t\t!QueryFormats(nvof::BufferUsageOutput, outputFormats)) {\n',
    '\t\tstd::vector<DXGI_FORMAT> inputFormats;\n'
    '\t\tstd::vector<DXGI_FORMAT> outputFormats;\n'
    '\t\tstd::vector<DXGI_FORMAT> costFormats;\n'
    '\t\tif (!QueryFormats(nvof::BufferUsageInput, inputFormats) ||\n'
    '\t\t\t\t!QueryFormats(nvof::BufferUsageOutput, outputFormats)) {\n')

replace_once(
    '\t\tif (std::find(outputFormats.begin(), outputFormats.end(), DXGI_FORMAT_R16G16_SINT) == outputFormats.end()) {\n'
    '\t\t\treturn Fail(std::format(\n'
    '\t\t\t\tL"Native NVOF R16G16_SINT flow output is unavailable; supported formats: {}",\n'
    '\t\t\t\tJoinFormats(outputFormats)));\n'
    '\t\t}\n\n\t\tnvof::InitParams init = {};\n',
    '\t\tif (std::find(outputFormats.begin(), outputFormats.end(), DXGI_FORMAT_R16G16_SINT) == outputFormats.end()) {\n'
    '\t\t\treturn Fail(std::format(\n'
    '\t\t\t\tL"Native NVOF R16G16_SINT flow output is unavailable; supported formats: {}",\n'
    '\t\t\t\tJoinFormats(outputFormats)));\n'
    '\t\t}\n\n'
    '\t\tcostCaptureEnabled = QueryFormats(nvof::BufferUsageCost, costFormats) &&\n'
    '\t\t\tstd::find(costFormats.begin(), costFormats.end(), DXGI_FORMAT_R8_UINT) != costFormats.end();\n\n'
    '\t\tnvof::InitParams init = {};\n')

replace_once(
    '\t\tinit.enableOutputCost = nvof::False;\n',
    '\t\tinit.enableOutputCost = costCaptureEnabled ? nvof::True : nvof::False;\n')

replace_once(
    '\t\tif (!CreateInputSurface(inputs[0]) || !CreateInputSurface(inputs[1]) ||\n'
    '\t\t\t\t!CreateFlowSurface(forwardFlow) || !CreateFlowSurface(backwardFlow) ||\n'
    '\t\t\t\t!CreateSynthesisResources()) {\n',
    '\t\tif (!CreateInputSurface(inputs[0]) || !CreateInputSurface(inputs[1]) ||\n'
    '\t\t\t\t!CreateFlowSurface(forwardFlow) || !CreateFlowSurface(backwardFlow) ||\n'
    '\t\t\t\t(costCaptureEnabled && (!CreateCostSurface(forwardCost) || !CreateCostSurface(backwardCost))) ||\n'
    '\t\t\t\t!CreateSynthesisResources()) {\n')

replace_once(
    '\t\truntimeInfo = std::format(\n'
    '\t\t\tL"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; renderer-owned synthesis",\n'
    '\t\t\tapiMajor, apiMinor);\n',
    '\t\truntimeInfo = std::format(\n'
    '\t\t\tL"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; renderer-owned synthesis; diagnostic cost {}",\n'
    '\t\t\tapiMajor, apiMinor, costCaptureEnabled ? L"R8_UINT" : L"unavailable");\n')

replace_once(
    '\t\toutput.outputBuffer = forwardFlow.nvofHandle;\n'
    '\t\toutput.backwardOutputBuffer = backwardFlow.nvofHandle;\n',
    '\t\toutput.outputBuffer = forwardFlow.nvofHandle;\n'
    '\t\toutput.backwardOutputBuffer = backwardFlow.nvofHandle;\n'
    '\t\tif (costCaptureEnabled) {\n'
    '\t\t\toutput.outputCostBuffer = forwardCost.nvofHandle;\n'
    '\t\t\toutput.backwardOutputCostBuffer = backwardCost.nvofHandle;\n'
    '\t\t}\n')

capture_block = '''\t\tDispatchMidpoint(midpointTime);

\t\tif (IsNativeNvofCaptureRequested()) {
\t\t\tNativeNvofCaptureInputs capture = {};
\t\t\tcapture.device = device;
\t\t\tcapture.context = context;
\t\t\tcapture.firstFrame = inputs[currentIndex].texture;
\t\t\tcapture.secondFrame = inputs[writeIndex].texture;
\t\t\tcapture.midpointFrame = outputTexture;
\t\t\tcapture.forwardFlow = forwardFlow.texture;
\t\t\tcapture.backwardFlow = backwardFlow.texture;
\t\t\tcapture.forwardCost = costCaptureEnabled ? forwardCost.texture.p : nullptr;
\t\t\tcapture.backwardCost = costCaptureEnabled ? backwardCost.texture.p : nullptr;
\t\t\tcapture.frameWidth = width;
\t\t\tcapture.frameHeight = height;
\t\t\tcapture.flowWidth = flowWidth;
\t\t\tcapture.flowHeight = flowHeight;
\t\t\tcapture.midpointTime = midpointTime;
\t\t\tcapture.firstTimestamp = previousTimestamp;
\t\t\tcapture.secondTimestamp = inputTimestamp;

\t\t\tstd::wstring captureDirectory;
\t\t\tstd::wstring captureError;
\t\t\tif (CaptureNativeNvofFramePair(capture, captureDirectory, captureError)) {
\t\t\t\tDLog(L"Native NVOF diagnostic capture saved to {}", captureDirectory);
\t\t\t} else {
\t\t\t\tDLog(L"Native NVOF diagnostic capture failed: {}", captureError);
\t\t\t}
\t\t}

'''
replace_once(
    '\t\tDispatchMidpoint(midpointTime);\n'
    '\t\tprocessTimeMs = std::chrono::duration<double, std::milli>(\n',
    capture_block + '\t\tprocessTimeMs = std::chrono::duration<double, std::milli>(\n')

path.write_text(source, encoding='utf-8')
print('Patched Source/NvidiaOpticalFlowNative.cpp')
