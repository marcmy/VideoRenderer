#pragma once

#include <d3d11.h>
#include <atlbase.h>
#include <array>

// Measures one GPU pass per frame with D3D11 timestamp queries. Results are
// collected a few frames later without flushing, so the CPU never waits.
class CGpuPassTimer
{
	struct Slot {
		CComPtr<ID3D11Query> disjoint, begin, end;
		bool pending = false;
	};
	std::array<Slot, 4> m_slots;
	UINT m_next = 0;
	Slot* m_active = nullptr;
	double m_averageMs = 0.0;
	UINT m_samples = 0;

	void Collect(ID3D11DeviceContext* context)
	{
		for (auto& slot : m_slots) {
			if (!slot.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
			if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
			UINT64 begin = 0, end = 0;
			slot.pending = false;
			if (disjoint.Disjoint || !disjoint.Frequency
					|| context->GetData(slot.begin, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
					|| context->GetData(slot.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
					|| end < begin) {
				continue;
			}
			const double ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
			// Smooth over roughly the last 30 samples; seed with the first one.
			m_averageMs = m_samples ? m_averageMs + (ms - m_averageMs) / 30.0 : ms;
			++m_samples;
		}
	}

public:
	void Release()
	{
		for (auto& slot : m_slots) slot = {};
		m_active = nullptr;
		m_next = 0;
		m_averageMs = 0.0;
		m_samples = 0;
	}

	void Begin(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		m_active = nullptr;
		Collect(context);
		auto& slot = m_slots[m_next];
		if (slot.pending) return; // The GPU is several frames behind; skip this sample.
		if (!slot.disjoint) {
			D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
			if (FAILED(device->CreateQuery(&desc, &slot.disjoint))) return;
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&desc, &slot.begin)) || FAILED(device->CreateQuery(&desc, &slot.end))) {
				slot = {};
				return;
			}
		}
		context->Begin(slot.disjoint);
		context->End(slot.begin);
		m_active = &slot;
	}

	void End(ID3D11DeviceContext* context)
	{
		if (!m_active) return;
		context->End(m_active->end);
		context->End(m_active->disjoint);
		m_active->pending = true;
		m_active = nullptr;
		m_next = (m_next + 1) % static_cast<UINT>(m_slots.size());
	}

	bool HasSample() const { return m_samples != 0; }
	double AverageMs() const { return m_averageMs; }
};
