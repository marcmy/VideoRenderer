#pragma once

#include <d3d11_4.h>

// CUDA's D3D11 interop calls touch the immediate context internally. Enabling
// SetMultithreadProtected alone does not serialize the whole map/unmap operation
// against other threads issuing D3D11 commands. Join the device's critical
// section explicitly, including registration and teardown. Do not hold it over
// TensorRT inference or stream completion waits.
class D3D11InteropLock final {
public:
    explicit D3D11InteropLock(ID3D11Multithread* multithread) noexcept
        : m_multithread(multithread)
    {
        if (m_multithread) m_multithread->Enter();
    }

    ~D3D11InteropLock()
    {
        if (m_multithread) m_multithread->Leave();
    }

    D3D11InteropLock(const D3D11InteropLock&) = delete;
    D3D11InteropLock& operator=(const D3D11InteropLock&) = delete;

private:
    ID3D11Multithread* m_multithread;
};
