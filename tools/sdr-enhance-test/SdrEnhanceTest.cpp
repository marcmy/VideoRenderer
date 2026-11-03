// SDR enhancement shader checks. Without arguments the production HLSL runs
// on WARP (no GPU needed). "--bench" times it on the hardware adapter.
#define NOMINMAX
#include <windows.h>
#include <atlbase.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

unsigned checks = 0;
void Check(bool ok, const char* label)
{
	++checks;
	if (!ok) throw std::runtime_error(label);
}

struct Float4 { float r, g, b, a; };
struct Params { float contrast = 0, pivot = 0.5f, vibrance = 0, skin = 0.75f, split = -1, divider = 1, pad0 = 0, pad1 = 0; };

const char* const kVertexShader = R"(
struct VS_OUTPUT { float4 Pos : SV_POSITION; float2 Tex : TEXCOORD; };
VS_OUTPUT main(uint id : SV_VertexID)
{
    VS_OUTPUT o;
    o.Tex = float2((id << 1) & 2, id & 2);
    o.Pos = float4(o.Tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
})";
// The renderer's passthrough (ps_simple) equivalent, for the baseline cost.
const char* const kCopyShader = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_Target { return tex.Sample(samp, uv); })";

CComPtr<ID3DBlob> Compile(const char* source, const wchar_t* file, const char* target)
{
	CComPtr<ID3DBlob> code, errors;
	const HRESULT hr = file
		? D3DCompileFromFile(file, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)
		: D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr)) {
		if (errors) fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
		throw std::runtime_error("shader compilation failed");
	}
	return code;
}

class Harness
{
public:
	CComPtr<ID3D11Device> device;
	CComPtr<ID3D11DeviceContext> context;
	CComPtr<ID3D11VertexShader> vs;
	CComPtr<ID3D11PixelShader> enhance, copy;
	CComPtr<ID3D11SamplerState> point;
	CComPtr<ID3D11Buffer> constants;

	explicit Harness(bool hardware, const std::wstring& shaderPath)
	{
		Check(SUCCEEDED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
			nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "D3D11 device");
		auto vsCode = Compile(kVertexShader, nullptr, "vs_4_0");
		auto psCode = Compile(nullptr, shaderPath.c_str(), "ps_4_0");
		auto copyCode = Compile(kCopyShader, nullptr, "ps_4_0");
		Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)), "VS");
		Check(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &enhance)), "PS");
		Check(SUCCEEDED(device->CreatePixelShader(copyCode->GetBufferPointer(), copyCode->GetBufferSize(), nullptr, &copy)), "copy PS");
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		Check(SUCCEEDED(device->CreateSamplerState(&sd, &point)), "sampler");
		D3D11_BUFFER_DESC bd = {sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
		Check(SUCCEEDED(device->CreateBuffer(&bd, nullptr, &constants)), "constants");
	}

	CComPtr<ID3D11Texture2D> Texture(UINT w, UINT h, DXGI_FORMAT format, const void* pixels, UINT pitch)
	{
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1; d.Format = format;
		d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_SUBRESOURCE_DATA data = {pixels, pitch, 0};
		CComPtr<ID3D11Texture2D> t;
		Check(SUCCEEDED(device->CreateTexture2D(&d, pixels ? &data : nullptr, &t)), "texture");
		return t;
	}

	void Draw(ID3D11ShaderResourceView* input, ID3D11RenderTargetView* target, UINT w, UINT h, ID3D11PixelShader* ps)
	{
		const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
		context->OMSetRenderTargets(1, &target, nullptr);
		context->RSSetViewports(1, &vp);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vs, nullptr, 0);
		context->PSSetShader(ps, nullptr, 0);
		context->PSSetShaderResources(0, 1, &input);
		context->PSSetSamplers(0, 1, &point.p);
		context->PSSetConstantBuffers(0, 1, &constants.p);
		context->Draw(3, 0);
	}

	// Runs the enhancement on float pixels and returns the float result.
	std::vector<Float4> Run(const std::vector<Float4>& pixels, UINT w, UINT h, const Params& params)
	{
		context->UpdateSubresource(constants, 0, nullptr, &params, 0, 0);
		auto in = Texture(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, pixels.data(), w * sizeof(Float4));
		auto out = Texture(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, 0);
		CComPtr<ID3D11ShaderResourceView> srv;
		CComPtr<ID3D11RenderTargetView> rtv;
		Check(SUCCEEDED(device->CreateShaderResourceView(in, nullptr, &srv)), "SRV");
		Check(SUCCEEDED(device->CreateRenderTargetView(out, nullptr, &rtv)), "RTV");
		Draw(srv, rtv, w, h, enhance);
		D3D11_TEXTURE2D_DESC d = {};
		out->GetDesc(&d);
		d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CComPtr<ID3D11Texture2D> staging;
		Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)), "staging");
		context->CopyResource(staging, out);
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		Check(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), "map");
		std::vector<Float4> result(size_t(w) * h);
		for (UINT y = 0; y < h; ++y) {
			memcpy(&result[size_t(y) * w], static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, w * sizeof(Float4));
		}
		context->Unmap(staging, 0);
		return result;
	}

	// Lays a colour list out in rows, within the D3D11 texture size limit.
	std::vector<Float4> Run(std::vector<Float4> pixels, const Params& params)
	{
		const size_t count = pixels.size();
		const UINT w = static_cast<UINT>(std::min<size_t>(count, 256));
		const UINT rows = static_cast<UINT>((count + w - 1) / w);
		pixels.resize(size_t(w) * rows, Float4{0, 0, 0, 1});
		auto out = Run(pixels, w, rows, params);
		out.resize(count);
		return out;
	}
};

// CPU OKLab for checks, identical to the shader's definition.
std::array<double, 3> Oklab(const Float4& p)
{
	auto lin = [](double v) { return std::pow(std::clamp(v, 0.0, 1.0), 2.2); };
	const double r = lin(p.r), g = lin(p.g), b = lin(p.b);
	const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
	const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
	const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
	return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
		1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
		0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}
double Chroma(const Float4& p) { const auto lab = Oklab(p); return std::hypot(lab[1], lab[2]); }
Float4 Grey(double v) { const float f = static_cast<float>(v); return {f, f, f, 1}; }

void CorrectnessTests(Harness& h)
{
	// Neutral settings reproduce every 8-bit code exactly and float input to 2e-4.
	std::vector<Float4> cube;
	for (int r = 0; r < 256; r += 5) for (int g = 0; g < 256; g += 5) for (int b = 0; b < 256; b += 5)
		cube.push_back({r / 255.0f, g / 255.0f, b / 255.0f, 1});
	for (int v = 0; v < 256; ++v) cube.push_back(Grey(v / 255.0));
	{
		const auto out = h.Run(cube, Params{});
		double worst = 0;
		for (size_t i = 0; i < cube.size(); ++i) {
			for (auto [a, b] : {std::pair{cube[i].r, out[i].r}, {cube[i].g, out[i].g}, {cube[i].b, out[i].b}}) {
				worst = std::max(worst, double(std::abs(a - b)));
				if (std::lround(a * 255.0) != std::lround(b * 255.0)) {
					fprintf(stderr, "in %.4f %.4f %.4f -> out %.6f %.6f %.6f\n",
						cube[i].r, cube[i].g, cube[i].b, out[i].r, out[i].g, out[i].b);
				}
				Check(std::lround(a * 255.0) == std::lround(b * 255.0), "neutral settings changed an 8-bit code");
			}
		}
		Check(worst < 2e-4, "neutral settings are not an identity in float");
		printf("neutral: %zu colours, max float error %.2e, all 8-bit codes preserved\n", cube.size(), worst);
	}

	// Grey ramp: monotonic, fixed ends and pivot, stays neutral, no clipping.
	std::vector<Float4> ramp;
	for (int i = 0; i <= 1023; ++i) ramp.push_back(Grey(i / 1023.0));
	for (float contrast : {-0.5f, -0.2f, 0.15f, 0.5f}) {
		for (float pivot : {0.05f, 0.2f, 0.5f, 0.95f}) {
			Params p; p.contrast = contrast; p.pivot = pivot;
			const auto out = h.Run(ramp, p);
			for (size_t i = 0; i < out.size(); ++i) {
				Check(std::abs(out[i].r - out[i].g) < 1e-4 && std::abs(out[i].g - out[i].b) < 1e-4, "tone curve tinted grey");
				Check(out[i].r >= -1e-6 && out[i].r <= 1 + 1e-6, "tone curve left [0, 1]");
				if (i) Check(out[i].r >= out[i - 1].r - 1e-5, "tone curve is not monotonic");
			}
			Check(std::abs(out.front().r) < 1e-4 && std::abs(out.back().r - 1) < 1e-4, "tone curve moved black or white");
			// Grey whose OKLab L equals the pivot: Y = pivot^3, encoded with 2.2.
			const auto pivotOut = h.Run({Grey(std::pow(std::pow(double(pivot), 3.0), 1 / 2.2))}, p);
			Check(std::abs(Oklab(pivotOut[0])[0] - pivot) < 2e-4, "tone curve moved the pivot");
			// Greys halfway below and above the pivot, in OKLab lightness.
			auto encode = [](double L) { return std::pow(L * L * L, 1 / 2.2); };
			const double below = encode(pivot * 0.5), above = encode(pivot + (1 - pivot) * 0.5);
			const auto mid = h.Run({Grey(below), Grey(above)}, p);
			const bool expanded = mid[0].r < below && mid[1].r > above;
			const bool compressed = mid[0].r > below && mid[1].r < above;
			Check(contrast > 0 ? expanded : compressed, "contrast did not act around the pivot");
		}
	}
	printf("tone curve: monotonic, black/white/pivot fixed, neutral, in range\n");

	// Strong settings on arbitrary colours stay finite, in gamut and hue-stable.
	std::mt19937 rng(1234);
	std::uniform_real_distribution<float> u(0, 1);
	std::vector<Float4> colours = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}, {0, 1, 1, 1}, {1, 0, 1, 1}};
	for (int i = 0; i < 8192; ++i) colours.push_back({u(rng), u(rng), u(rng), 1});
	for (Params p : {Params{0.15f, 0.2f, 0.25f, 0.5f}, Params{0.5f, 0.05f, 1.0f, 0}, Params{0.5f, 0.95f, 1.0f, 0}, Params{0.5f, 0.5f, 1.0f, 0}, Params{-0.5f, 0.3f, 1.0f, 1}, Params{0.5f, 0.7f, -0.5f, 0}}) {
		const auto out = h.Run(colours, p);
		double worstHue = 0;
		for (size_t i = 0; i < out.size(); ++i) {
			for (float c : {out[i].r, out[i].g, out[i].b}) Check(std::isfinite(c) && c >= 0 && c <= 1, "output left the SDR gamut");
			const auto a = Oklab(colours[i]), b = Oklab(out[i]);
			const double ca = std::hypot(a[1], a[2]), cb = std::hypot(b[1], b[2]);
			if (ca > 0.05 && cb > 0.05) {
				double d = std::abs(std::atan2(a[2], a[1]) - std::atan2(b[2], b[1]));
				worstHue = std::max(worstHue, std::min(d, 6.283185307 - d));
			}
			if (p.vibrance < 0) Check(cb <= ca * (b[0] / std::max(a[0], 1e-3)) + 2e-3, "negative vibrance added chroma");
		}
		// Tone, vibrance and gamut mapping all keep OKLab hue; only rounding
		// and the 2.2 encode of near-zero channels may move it slightly.
		Check(worstHue < 0.035, "hue shifted by more than 2 degrees");
		printf("gamut: contrast %+.2f vibrance %+.2f -> in range, worst hue shift %.2f deg\n", p.contrast, p.vibrance, worstHue * 57.2958);
	}

	// Vibrance favours muted colours, and skin protection limits it.
	{
		Params p; p.vibrance = 0.5f; p.skin = 0;
		const std::vector<Float4> in = {{0.55f, 0.5f, 0.45f, 1}, {0.85f, 0.25f, 0.15f, 1}, {0.85f, 0.65f, 0.55f, 1}, {0.5f, 0.5f, 0.5f, 1}};
		const auto out = h.Run(in, p);
		p.skin = 1;
		const auto protectedOut = h.Run(in, p);
		const double muted = Chroma(out[0]) / Chroma(in[0]), saturated = Chroma(out[1]) / Chroma(in[1]);
		const double skin = Chroma(out[2]) / Chroma(in[2]), skinProtected = Chroma(protectedOut[2]) / Chroma(in[2]);
		Check(muted > saturated && muted > 1.05, "vibrance does not favour muted colours");
		Check(skinProtected < skin, "skin protection had no effect");
		Check(Chroma(out[3]) < 1e-4, "vibrance tinted neutral grey");
		printf("vibrance: muted x%.3f, saturated x%.3f, skin x%.3f (protected x%.3f)\n", muted, saturated, skin, skinProtected);
	}

	// A/B split: unprocessed left of the divider, identical processing right.
	{
		const UINT w = 64, hgt = 4;
		std::vector<Float4> image(size_t(w) * hgt);
		for (size_t i = 0; i < image.size(); ++i) image[i] = {0.2f + (i % w) * 0.01f, 0.5f, 0.35f, 1};
		Params full; full.contrast = 0.4f; full.vibrance = 0.6f;
		Params split = full; split.split = 32; split.divider = 1;
		const auto a = h.Run(image, w, hgt, full), b = h.Run(image, w, hgt, split);
		for (UINT y = 0; y < hgt; ++y) for (UINT x = 0; x < w; ++x) {
			const size_t i = size_t(y) * w + x;
			if (x < 31) Check(memcmp(&b[i], &image[i], sizeof(Float4)) == 0, "A/B reference half was modified");
			else if (x == 31) Check(b[i].r == 0.5f && b[i].g == 0.5f && b[i].b == 0.5f, "A/B divider missing");
			else Check(memcmp(&b[i], &a[i], sizeof(Float4)) == 0, "A/B processed half differs from full processing");
		}
		printf("A/B split: reference half bit-exact, processed half identical to On\n");
	}
}

struct Timing { double copyUs, enhanceUs; };

uint16_t ToHalf(float v)
{
	// Non-negative values below 2.0 only; tiny values flush to zero.
	if (v < 6.104e-5f) return 0;
	uint32_t bits;
	memcpy(&bits, &v, sizeof(bits));
	return static_cast<uint16_t>(((bits >> 23) - 112) << 10 | ((bits >> 13) & 0x3ff));
}

// "natural": smooth, mostly muted content with a few saturated areas, as in
// typical video. "noise": random colours, the worst case for gamut mapping.
std::vector<Float4> Content(UINT w, UINT hgt, bool natural)
{
	std::vector<Float4> pixels(size_t(w) * hgt);
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> u(0, 1);
	for (UINT y = 0; y < hgt; ++y) for (UINT x = 0; x < w; ++x) {
		Float4& p = pixels[size_t(y) * w + x];
		if (!natural) { p = {u(rng), u(rng), u(rng), 1}; continue; }
		const double fx = double(x) / w, fy = double(y) / hgt;
		const double L = 0.55 + 0.3 * std::sin(fx * 7.1 + 1.3) * std::cos(fy * 5.3) + 0.01 * (u(rng) - 0.5);
		const double hue = fx * 4.0 + fy * 2.5;
		const bool vivid = std::sin(fx * 23.0) * std::sin(fy * 17.0) > 0.85; // about 8% of the frame
		const double C = vivid ? 0.2 : 0.02 + 0.06 * (0.5 + 0.5 * std::sin(fx * 3.0 + fy * 9.0));
		const double a = C * std::cos(hue), b = C * std::sin(hue);
		double l = L + 0.3963377774 * a + 0.2158037573 * b, m = L - 0.1055613458 * a - 0.0638541728 * b,
			s = L - 0.0894841775 * a - 1.2914855480 * b;
		l = l * l * l; m = m * m * m; s = s * s * s;
		auto enc = [](double v) { return static_cast<float>(std::pow(std::clamp(v, 0.0, 1.0), 1 / 2.2)); };
		p = {enc(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
			enc(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
			enc(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s), 1};
	}
	return pixels;
}

Timing Benchmark(Harness& h, UINT w, UINT hgt, DXGI_FORMAT inFormat, DXGI_FORMAT outFormat, bool natural)
{
	const auto content = Content(w, hgt, natural);
	const UINT bpp = inFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
	std::vector<uint8_t> pixels(size_t(w) * hgt * bpp);
	for (size_t i = 0; i < content.size(); ++i) {
		const float c[4] = {content[i].r, content[i].g, content[i].b, 1};
		if (bpp == 8) {
			auto* half = reinterpret_cast<uint16_t*>(pixels.data()) + i * 4;
			for (int k = 0; k < 4; ++k) half[k] = ToHalf(c[k]);
		} else {
			uint8_t* q = pixels.data() + i * 4; // BGRA
			q[0] = uint8_t(std::lround(c[2] * 255)); q[1] = uint8_t(std::lround(c[1] * 255));
			q[2] = uint8_t(std::lround(c[0] * 255)); q[3] = 255;
		}
	}
	auto in = h.Texture(w, hgt, inFormat, pixels.data(), w * bpp);
	auto out = h.Texture(w, hgt, outFormat, nullptr, 0);
	CComPtr<ID3D11ShaderResourceView> srv;
	CComPtr<ID3D11RenderTargetView> rtv;
	Check(SUCCEEDED(h.device->CreateShaderResourceView(in, nullptr, &srv)), "bench SRV");
	Check(SUCCEEDED(h.device->CreateRenderTargetView(out, nullptr, &rtv)), "bench RTV");
	Params p; p.contrast = 0.15f; p.pivot = 0.2f; p.vibrance = 0.25f; p.skin = 0.5f; // the defaults
	h.context->UpdateSubresource(h.constants, 0, nullptr, &p, 0, 0);

	D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
	auto measure = [&](ID3D11PixelShader* ps) {
		constexpr int draws = 400, runs = 7;
		for (int i = 0; i < 100; ++i) h.Draw(srv, rtv, w, hgt, ps); // warm-up and clock ramp
		std::vector<double> perDraw;
		for (int run = 0; run < runs; ++run) {
			CComPtr<ID3D11Query> disjoint, t0, t1;
			qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; h.device->CreateQuery(&qd, &disjoint);
			qd.Query = D3D11_QUERY_TIMESTAMP; h.device->CreateQuery(&qd, &t0); h.device->CreateQuery(&qd, &t1);
			h.context->Begin(disjoint);
			h.context->End(t0);
			for (int i = 0; i < draws; ++i) h.Draw(srv, rtv, w, hgt, ps);
			h.context->End(t1);
			h.context->End(disjoint);
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
			while (h.context->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) Sleep(0);
			UINT64 a = 0, b = 0;
			while (h.context->GetData(t0, &a, sizeof(a), 0) != S_OK) Sleep(0);
			while (h.context->GetData(t1, &b, sizeof(b), 0) != S_OK) Sleep(0);
			Check(!dj.Disjoint, "timestamps disjoint");
			perDraw.push_back(double(b - a) * 1e6 / double(dj.Frequency) / draws);
		}
		std::sort(perDraw.begin(), perDraw.end());
		return perDraw[perDraw.size() / 2];
	};
	const double copyUs = measure(h.copy);
	const double enhanceUs = measure(h.enhance);
	return {copyUs, enhanceUs};
}

const char* FormatName(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? "16F" : f == DXGI_FORMAT_R10G10B10A2_UNORM ? "10-bit" : "8-bit";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	try {
		bool bench = false;
		std::wstring shader = L"Shaders\\d3d11\\ps_sdr_enhance.hlsl";
		for (int i = 1; i < argc; ++i) {
			if (std::wstring(argv[i]) == L"--bench") bench = true;
			else shader = argv[i];
		}
		Harness h(bench, shader);
		if (!bench) {
			CorrectnessTests(h);
			printf("PASS %u SDR enhancement checks on WARP\n", checks);
			return 0;
		}

		CComPtr<IDXGIDevice> dxgi;
		CComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc = {};
		if (SUCCEEDED(h.device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&desc);
		printf("adapter: %ls\n", desc.Description);
		printf("%-10s %-8s %-14s %9s %11s\n", "size", "content", "in -> out", "copy us", "enhance us");
		const std::pair<UINT, UINT> sizes[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
		// With the enhancement on, the post-scale chain is 16F; the last step
		// may write the 8-bit swap chain when dithering is disabled.
		const std::pair<DXGI_FORMAT, DXGI_FORMAT> formats[] = {
			{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT},
			{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM},
		};
		for (auto [w, hgt] : sizes) {
			for (bool natural : {true, false}) {
				for (auto [fi, fo] : formats) {
					const auto t = Benchmark(h, w, hgt, fi, fo, natural);
					char io[32];
					snprintf(io, sizeof(io), "%s -> %s", FormatName(fi), FormatName(fo));
					printf("%4ux%-5u %-8s %-14s %9.1f %11.1f\n", w, hgt, natural ? "natural" : "noise", io, t.copyUs, t.enhanceUs);
				}
			}
		}
		return 0;
	} catch (const std::exception& e) {
		fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
