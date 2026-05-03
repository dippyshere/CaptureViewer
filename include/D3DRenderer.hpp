#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

class D3DRenderer {
public:
    enum class FrameFormat {
        BGRA8,
        NV12,
    };

    D3DRenderer() = default;
    ~D3DRenderer();

    bool initialize(HWND hwnd);
    void shutdown();

    void onResize(UINT width, UINT height);

    void uploadFrame(const void* data,
                     std::size_t dataSize,
                     std::uint32_t stride,
                     std::uint32_t width,
                     std::uint32_t height,
                     FrameFormat format);

    void render(const std::function<void(ID3D12GraphicsCommandList*)>& overlayCallback = nullptr);

    void setVSyncEnabled(bool enable) { vsyncEnabled_ = enable; }
    [[nodiscard]] bool vsyncEnabled() const { return vsyncEnabled_; }

    [[nodiscard]] ID3D12Device* device() const { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* commandQueue() const { return commandQueue_.Get(); }
    [[nodiscard]] ID3D12DescriptorHeap* srvHeap() const { return srvHeap_.Get(); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE frameSrvCpuHandle() const { return srvHandleFrameCpu_; }
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE frameSrvGpuHandle() const { return srvHandleFrameGpu_; }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE imguiSrvCpuHandle() const { return srvHandleImGuiCpu_; }
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE imguiSrvGpuHandle() const { return srvHandleImGuiGpu_; }
    [[nodiscard]] DXGI_FORMAT renderTargetFormat() const { return DXGI_FORMAT_B8G8R8A8_UNORM; }
    [[nodiscard]] UINT frameCount() const { return kFrameCount; }
    [[nodiscard]] UINT srvDescriptorSize() const { return srvDescriptorSize_; }

    [[nodiscard]] HANDLE frameLatencyWaitableObject() const { return frameLatencyWaitableObject_; }

    void setViewportRect(float x, float y, float width, float height);
    void setBlurEnabled(bool enabled) { blurEnabled_ = enabled; }

private:
    struct FrameContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        std::uint64_t fenceValue = 0;
    };

    bool createDevice(HWND hwnd);
    bool createSwapChain(HWND hwnd);
    bool createPipelineResources();
    bool createRenderTargets();
    void destroyRenderTarget();
    bool ensureFrameResources(std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t stride,
                              FrameFormat format);
    void destroyFrameResources();
    void waitForFrame(FrameContext& frameContext);
    void waitForGpu();

    static constexpr std::uint32_t kFrameCount = 2;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerHeap_;

    Microsoft::WRL::ComPtr<ID3D12Resource> renderTargets_[kFrameCount];
    FrameContext frameContexts_[kFrameCount];

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    std::uint64_t fenceValue_ = 1;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateNv12_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateGradient_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateBlur_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineStateNv12Blur_;

    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView_{};

    struct UploadResource {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2]{};
        UINT rowCounts[2]{};
        std::uint64_t rowSizes[2]{};
        UINT subresourceCount = 0;
        std::uint64_t sizeBytes = 0;
        std::uint8_t* cpuAddress = nullptr;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> frameTexture_;
    UploadResource frameUploads_[kFrameCount];
    bool pendingUpload_[kFrameCount] = {};

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandleStart_{};
    UINT srvDescriptorSize_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandleFrameCpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandleFrameGpu_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandleFramePlane1Cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandleFramePlane1Gpu_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandleImGuiCpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandleImGuiGpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE samplerHandleGpu_{};

    UINT rtvDescriptorSize_ = 0;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissorRect_{};

    UINT frameWidth_ = 0;
    UINT frameHeight_ = 0;
    UINT frameStride_ = 0;
    FrameFormat frameFormat_ = FrameFormat::BGRA8;
    UINT backBufferWidth_ = 0;
    UINT backBufferHeight_ = 0;

    HANDLE frameLatencyWaitableObject_ = nullptr;
    bool allowTearing_ = false;
    bool vsyncEnabled_ = false;
    bool debugGradient_ = false;
    bool loggedGpuPixels_ = false;
    bool debugLayerEnabled_ = false;
    bool blurEnabled_ = false;

    void updateViewport(UINT width, UINT height);
};
