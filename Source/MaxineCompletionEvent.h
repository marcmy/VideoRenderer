#pragma once

#include <windows.h>

// Serialized by the Maxine caller. A timeout leaves the event quarantined until
// its original GPU completion is consumed; it must not complete a newer input.
class CMaxineCompletionEvent final
{
public:
	~CMaxineCompletionEvent() { Clear(); }
	CMaxineCompletionEvent() = default;
	CMaxineCompletionEvent(const CMaxineCompletionEvent&) = delete;
	CMaxineCompletionEvent& operator=(const CMaxineCompletionEvent&) = delete;

	HRESULT Acquire(HANDLE& ready)
	{
		ready = nullptr;
		if (m_pending) {
			const HRESULT hr = Wait(0);
			if (FAILED(hr)) return hr;
		}
		if (!m_event) {
			m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!m_event) return HRESULT_FROM_WIN32(GetLastError());
		}
		if (!ResetEvent(m_event)) return HRESULT_FROM_WIN32(GetLastError());
		ready = m_event;
		return S_OK;
	}

	void MarkPending() { m_pending = true; }

	HRESULT Wait(const DWORD timeoutMs)
	{
		if (!m_event || !m_pending) return E_UNEXPECTED;
		const DWORD result = WaitForSingleObject(m_event, timeoutMs);
		if (result == WAIT_OBJECT_0) {
			m_pending = false;
			return S_OK;
		}
		const DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
		return HRESULT_FROM_WIN32(result == WAIT_TIMEOUT ? ERROR_TIMEOUT : error);
	}

	void Clear()
	{
		if (m_event) CloseHandle(m_event);
		m_event = nullptr;
		m_pending = false;
	}

private:
	HANDLE m_event = nullptr;
	bool m_pending = false;
};
