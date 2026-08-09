from pathlib import Path

path = Path('Source/NvidiaOpticalFlowNative.cpp')
s = path.read_text(encoding='utf-8')

old = '''\t\tstd::vector<DXGI_FORMAT> costFormats;\n\t\tif (!QueryFormats(nvof::BufferUsageInput, inputFormats) ||\n'''
new = '''\t\tif (!QueryFormats(nvof::BufferUsageInput, inputFormats) ||\n'''
if old not in s:
    raise SystemExit('costFormats declaration target not found')
s = s.replace(old, new, 1)

old = '''\n\t\tcostCaptureEnabled = QueryFormats(nvof::BufferUsageCost, costFormats) &&\n\t\t\tstd::find(costFormats.begin(), costFormats.end(), DXGI_FORMAT_R8_UINT) != costFormats.end();\n'''
new = '''\n\t\t// Hardware cost output is intentionally disabled in the live renderer.\n\t\t// On Turing it can severely stall startup/seeks and sustained playback.\n\t\t// Real-frame capture still records source frames, flow, consistency, and midpoint.\n\t\tcostCaptureEnabled = false;\n'''
if old not in s:
    raise SystemExit('cost capability target not found')
s = s.replace(old, new, 1)

old = '''\t\tinit.enableOutputCost = costCaptureEnabled ? nvof::True : nvof::False;\n'''
new = '''\t\tinit.enableOutputCost = nvof::False;\n'''
if old not in s:
    raise SystemExit('enableOutputCost target not found')
s = s.replace(old, new, 1)

old = '''\t\truntimeInfo = std::format(\n\t\t\tL\"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; renderer-owned synthesis; diagnostic cost {}\",\n\t\t\tapiMajor, apiMinor, costCaptureEnabled ? L\"R8_UINT\" : L\"unavailable\");\n'''
new = '''\t\truntimeInfo = std::format(\n\t\t\tL\"Driver NVOF {}.{}; D3D11; BGRA8; 4x4 bidirectional flow; renderer-owned synthesis; live cost disabled\",\n\t\t\tapiMajor, apiMinor);\n'''
if old not in s:
    raise SystemExit('runtimeInfo target not found')
s = s.replace(old, new, 1)

path.write_text(s, encoding='utf-8')
