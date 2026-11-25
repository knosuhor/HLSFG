// UI recomposition pass: generated frames get the current frame's UI
// contribution layered on top of the interpolated world so HUD and
// menu elements never warp or strobe. Two modes:
//   mode 0 (hudless): reference is the game's HUDLessColor,
//     ui = (present - hudless) * smoothstep(cutoff, 2*cutoff, delta),
//     out = interp + ui
//   mode 1 (temporal, broken or missing hudless): reference is the
//     previous backbuffer; pixels that did not change between real
//     frames are UI (or static content), so
//     mask = 1 - smoothstep(cutoff, 2*cutoff, |present - prev|),
//     out = lerp(interp, present, mask)
//     and the HUD is taken sharp from the current real frame.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace mfg
{
class UiComposite
{
  public:
    // Compiles the compute shader and builds the root signature / PSO.
    // Returns false on any failure (caller disables compositing).
    bool Init(ID3D12Device* device);
    bool Ready() const { return m_ready; }

    // hudless: current HUDLess frame (mode 0) or previous backbuffer
    //   (mode 1) in UAV state on entry (pipeline ring).
    // present: current backbuffer with UI, assumed NON_PIXEL_SHADER_RESOURCE.
    // interp: OutputInterpolated, UNORDERED_ACCESS on entry and exit.
    // Cutoff: percent-scale threshold, e.g. 0.02 for the default 2%.
    void Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12Resource* hudless,
        ID3D12Resource* present, ID3D12Resource* interp,
        uint32_t width, uint32_t height, float cutoff, uint32_t mode);

    void Release();

  private:
    bool EnsureScratch(const D3D12_RESOURCE_DESC& interpDesc);

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_scratch;
    UINT m_heapInc = 0;
    uint64_t m_scratchW = 0;
    uint64_t m_scratchH = 0;
    DXGI_FORMAT m_scratchFmt = DXGI_FORMAT_UNKNOWN;
    bool m_ready = false;
    bool m_warned = false;
};
} // namespace mfg

