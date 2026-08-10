$ErrorActionPreference = 'Stop'

$py = @'
from pathlib import Path

path = Path('Source/NvidiaOpticalFlowNative.cpp')
s = path.read_text(encoding='utf-8')

s = s.replace('#include <d3dcompiler.h>\n', '', 1)

needle = 'constexpr UINT FlowGridSize = 4;\n'
replacement = '''constexpr UINT FlowGridSize = 4;\nconstexpr int CapsSupportedOutputGridSizes = 0;\n'''
if needle not in s:
    raise RuntimeError('FlowGridSize marker not found')
s = s.replace(needle, replacement, 1)

needle = '''std::wstring JoinFormats(const std::vector<DXGI_FORMAT>& formats)\n{\n\tstd::wstring result;\n\tfor (const auto format : formats) {\n\t\tif (!result.empty()) {\n\t\t\tresult.append(L", ");\n\t\t}\n\t\tresult.append(FormatName(format));\n\t}\n\treturn result.empty() ? L"none" : result;\n}\n'''
replacement = needle + '''\nstd::wstring JoinGridSizes(const std::vector<uint32_t>& grids)\n{\n\tstd::wstring result;\n\tfor (const auto grid : grids) {\n\t\tif (!result.empty()) {\n\t\t\tresult.append(L", ");\n\t\t}\n\t\tresult.append(std::format(L"{}x{}", grid, grid));\n\t}\n\treturn result.empty() ? L"none" : result;\n}\n'''
if needle not in s:
    raise RuntimeError('JoinFormats block not found')
s = s.replace(needle, replacement, 1)

needle = '''\tbool QueryFormats(const nvof::BufferUsage usage, std::vector<DXGI_FORMAT>& formats)\n\t{\n\t\tuint32_t count = 0;\n\t\tnvof::Status code = api.getSurfaceFormatCountD3D11(\n\t\t\tsession, usage, nvof::ModeOpticalFlow, &count);\n\t\tif (code != nvof::Success) {\n\t\t\treturn false;\n\t\t}\n\t\tformats.assign(count, DXGI_FORMAT_UNKNOWN);\n\t\tif (!count) {\n\t\t\treturn true;\n\t\t}\n\t\tcode = api.getSurfaceFormatD3D11(\n\t\t\tsession, usage, nvof::ModeOpticalFlow, formats.data());\n\t\treturn code == nvof::Success;\n\t}\n'''
replacement = needle + '''\n\tbool QueryCapability(const int capability, std::vector<uint32_t>& values)\n\t{\n\t\tif (!api.getCaps) {\n\t\t\treturn false;\n\t\t}\n\t\tuint32_t count = 0;\n\t\tnvof::Status code = api.getCaps(session, capability, nullptr, &count);\n\t\tif (code != nvof::Success) {\n\t\t\treturn false;\n\t\t}\n\t\tvalues.assign(count, 0);\n\t\tif (!count) {\n\t\t\treturn true;\n\t\t}\n\t\tcode = api.getCaps(session, capability, values.data(), &count);\n\t\tif (code != nvof::Success) {\n\t\t\treturn false;\n\t\t}\n\t\tvalues.resize(count);\n\t\treturn true;\n\t}\n'''
if needle not in s:
    raise RuntimeError('QueryFormats block not found')
s = s.replace(needle, replacement, 1)

old = '''\t\tif (code != nvof::Success || !api.createOpticalFlowD3D11 ||\n\t\t\t\t!api.initialize || !api.getSurfaceFormatCountD3D11 ||\n\t\t\t\t!api.getSurfaceFormatD3D11 || !api.registerResourceD3D11 ||\n\t\t\t\t!api.unregisterResourceD3D11 || !api.execute || !api.destroy) {\n'''
new = '''\t\tif (code != nvof::Success || !api.createOpticalFlowD3D11 ||\n\t\t\t\t!api.initialize || !api.getSurfaceFormatCountD3D11 ||\n\t\t\t\t!api.getSurfaceFormatD3D11 || !api.registerResourceD3D11 ||\n\t\t\t\t!api.unregisterResourceD3D11 || !api.execute || !api.destroy || !api.getCaps) {\n'''
if old not in s:
    raise RuntimeError('Function table validation target not found')
s = s.replace(old, new, 1)

needle = '''\t\tcode = api.createOpticalFlowD3D11(device, context, &session);\n\t\tif (code != nvof::Success || !session) {\n\t\t\treturn Fail(std::format(L"NvCreateOpticalFlowD3D11 failed: {}", DriverError(code)));\n\t\t}\n\n\t\tstd::vector<DXGI_FORMAT> inputFormats;\n'''
replacement = '''\t\tcode = api.createOpticalFlowD3D11(device, context, &session);\n\t\tif (code != nvof::Success || !session) {\n\t\t\treturn Fail(std::format(L"NvCreateOpticalFlowD3D11 failed: {}", DriverError(code)));\n\t\t}\n\n\t\tstd::vector<uint32_t> outputGridSizes;\n\t\tif (!QueryCapability(CapsSupportedOutputGridSizes, outputGridSizes)) {\n\t\t\treturn Fail(L"Could not query native NVOF output-grid capabilities");\n\t\t}\n\t\tif (std::find(outputGridSizes.begin(), outputGridSizes.end(), FlowGridSize) == outputGridSizes.end()) {\n\t\t\treturn Fail(std::format(\n\t\t\t\tL"Native NVOF 4x4 flow is required by the validated synthesis pipeline; GPU/driver supports: {}",\n\t\t\t\tJoinGridSizes(outputGridSizes)));\n\t\t}\n\n\t\tstd::vector<DXGI_FORMAT> inputFormats;\n'''
if needle not in s:
    raise RuntimeError('Session creation insertion target not found')
s = s.replace(needle, replacement, 1)

old = '''\t\truntimeInfo = std::format(\n\t\t\tL"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; validated jump-flood dense flow + edge-aware next-frame warp; live cost disabled",\n\t\t\tapiMajor, apiMinor);\n'''
new = '''\t\truntimeInfo = std::format(\n\t\t\tL"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow (GPU grids: {}); validated jump-flood dense flow + edge-aware next-frame warp + frame quality gate; live cost disabled",\n\t\t\tapiMajor, apiMinor, JoinGridSizes(outputGridSizes));\n'''
if old not in s:
    raise RuntimeError('runtimeInfo target not found')
s = s.replace(old, new, 1)

path.write_text(s, encoding='utf-8')

proj = Path('Source/MpcVideoRenderer.vcxproj')
p = proj.read_text(encoding='utf-8-sig')
p = p.replace('<AdditionalDependencies>d3dcompiler.lib;%(AdditionalDependencies)</AdditionalDependencies>\n', '', 1)
proj.write_text(p, encoding='utf-8-sig')
'@

$py | python -
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git add Source/NvidiaOpticalFlowNative.cpp Source/MpcVideoRenderer.vcxproj
git commit -m 'Harden native NVOF GPU capability handling'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
git push origin HEAD:feature/native-nvof-interpolation
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
