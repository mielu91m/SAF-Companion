#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <cstdint>

namespace SAFCompanion::Overlay
{
    bool InitD3D12Hook();
    void ShutdownD3D12Hook();
    bool IsOverlayVisible();
    void SetOverlayVisible(bool visible);
    void ToggleOverlay();
}
