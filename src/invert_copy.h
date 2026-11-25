// Invert copy pass for complemented hudless inputs.
//
// Some games (measured: Cyberpunk 2077 through OptiScaler) tag their
// HUDLessColor buffer with every byte complemented: texel = 255 - color.
// Interpolating that input directly feeds the LSFG graph a negative of
// the scene. This pass moves the incoming hudless frame into the ring
// with rgb negated (1 - x) and alpha passed through, so the graph and
// the UI delta both see true colors.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace mfg
{
class InvertCopy final
{
  public:
    bool Init(ID3D12Device* device);
    bool Ready() const { return m_ready; }
    // src is transitioned srcBefore -> NON_PIXEL_SHADER_RESOURCE for the
    // read and back to srcAfter; dst stays in UNORDERED_ACCESS for the
    // typed store, matching the ring's idle state.
    void Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src,
        D3D12_RESOURCE_STATES srcBefore, D3D12_RESOURCE_STATES srcAfter,
        ID3D12Resource* dst);
    void Release();

  private:
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_heapInc = 0;
    bool m_ready = false;
};
} // namespace mfg
