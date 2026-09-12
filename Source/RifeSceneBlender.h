#pragma once

#include <memory>
#include <d3d11.h>

// Lightweight GPU-only A/B blender used only when a scene cut is detected and
// the user selected "Blend adjacent frames". It uses a deferred D3D11 context
// so worker-thread state cannot contaminate MPCVR's immediate rendering state.
class CRifeSceneBlender
{
public:
    CRifeSceneBlender();
    ~CRifeSceneBlender();

    CRifeSceneBlender(const CRifeSceneBlender&) = delete;
    CRifeSceneBlender& operator=(const CRifeSceneBlender&) = delete;

    bool Blend(
        ID3D11Device* device,
        ID3D11Texture2D* first,
        ID3D11Texture2D* second,
        ID3D11Texture2D* output,
        float timestep);

    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
