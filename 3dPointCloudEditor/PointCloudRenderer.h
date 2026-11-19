#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "PlyLoader.h"
#include "SelectionVolume.h"

namespace app
{
    // 基于 DirectX 12 的点云渲染器。
    // 顶点以 POINTLIST 提交，由几何着色器把每个点扩展成面向屏幕的圆点四边形。
    class PointCloudRenderer
    {
    public:
        PointCloudRenderer() = default;
        ~PointCloudRenderer();

        PointCloudRenderer(PointCloudRenderer const&) = delete;
        PointCloudRenderer& operator=(PointCloudRenderer const&) = delete;

        // 创建设备、交换链并绑定到 SwapChainPanel。
        void Initialize(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);

        // 调整交换链尺寸（像素）。尺寸未变化时为空操作。
        void Resize(std::uint32_t pixelWidth, std::uint32_t pixelHeight);

        // 上传点云并替换当前内容（同步等待 GPU 完成）。
        void SetPoints(std::vector<PlyVertex> const& vertices);

        // 绘制一帧。
        void Render(DirectX::XMFLOAT4X4 const& viewProjection, float pointSizePixels);

        // 设置视口背景色（浅色主题时使用浅灰）。
        void SetClearColor(float r, float g, float b)
        {
            m_clearColor[0] = r;
            m_clearColor[1] = g;
            m_clearColor[2] = b;
            m_clearColor[3] = 1.0f;
        }

        // 设置/隐藏多边形选择体（棱柱）：多边形平面 + 沿视线方向的长度。
        // extruded=true 时画前后两个多边形和连接边；false 时只画多边形轮廓（绘制过程中）。
        // selected=true 时线框加粗。
        void SetSelectionPrism(
            bool visible,
            SelectionFrame const& frame,
            std::vector<DirectX::XMFLOAT2> const& polygon,
            float length,
            bool extruded,
            bool selected);

        // 设置/隐藏点云聚类选择（红色覆盖绘制的选中点集）。
        void SetClusterSelection(bool visible, DirectX::XMFLOAT3 const& center, float radius);

        // 用红色点绘制聚类选中的点集（与主点云位置相同，深度 LESS_EQUAL 保证可见）。
        void SetClusterPoints(std::vector<PlyVertex> const& points);

        // 世界坐标轴示意线：从 center 出发沿 ±X（红）、±Y（绿）、±Z（蓝），
        // 正向长度 length、负向长度 length/2；关闭深度测试，始终可见。
        void SetAxes(DirectX::XMFLOAT3 const& center, float length, bool visible);

        // 读取深度缓冲区在 (x, y) 处的深度值（NDC，0=近平面，1=远平面）。
        // 用于把多边形平面锚定到光标下的点云表面。
        bool PickDepth(std::uint32_t x, std::uint32_t y, float& ndcDepth);

        // 等待 GPU 执行完已提交的命令。
        void WaitForGpu();

        void Shutdown();

        bool IsReady() const noexcept { return m_swapChain != nullptr && m_width > 0 && m_height > 0; }
        std::uint32_t Width() const noexcept { return m_width; }
        std::uint32_t Height() const noexcept { return m_height; }
        std::size_t PointCount() const noexcept { return m_vertexCount; }
        std::wstring const& AdapterName() const noexcept { return m_adapterName; }

    private:
        static constexpr std::uint32_t kBufferCount = 2;
        static constexpr std::uint32_t kRootConstantsCount = 24; // 矩阵(16) + 4 float + 线偏移(4)

        float m_clearColor[4]{ 0.055f, 0.063f, 0.078f, 1.0f };

        struct SceneConstants
        {
            DirectX::XMFLOAT4X4 viewProjection;
            float pointSize;
            float viewportWidth;
            float viewportHeight;
            float padding;
            float lineOffsetX;
            float lineOffsetY;
            float padding2;
            float padding3;
        };

        static constexpr std::uint32_t kMaxPolygonVertices = 32;

        // 多边形选择体数据，通过根 CBV（b1）传给顶点着色器。
        // 注意：HLSL 常量缓冲区里的数组元素按 16 字节对齐，所以这里每个顶点用
        // XMFLOAT4 存（只用到 x/y），与着色器里的 float2 gPolygon[32] 布局一致。
        struct SelectionConstants
        {
            DirectX::XMFLOAT4 originAndLength; // xyz = 平面原点, w = 沿视线的长度
            DirectX::XMFLOAT4 right;               // xyz = right, w = 是否可见(0/1)
            DirectX::XMFLOAT4 up;
            DirectX::XMFLOAT4 forward;
            DirectX::XMFLOAT4 polygon[kMaxPolygonVertices];
            std::uint32_t polygonCount;
            float padding0;
            float padding1;
            float padding2;
            // 点云聚类选择：以种子点为中心、半径为 radius 的球内点显示为红色
            std::uint32_t clusterEnabled;
            std::uint32_t clusterPadding0;
            float clusterPadding1;
            float clusterPadding2;
            DirectX::XMFLOAT4 clusterCenterAndRadius; // xyz = 中心, w = 半径
        };

        void CreateDevice();
        void CreateCommandObjects();
        void CreatePipelineState();
        void CreateSwapChain();
        void CreateRenderTargets();
        void ReleaseRenderTargets();
        void AttachSwapChain();
        void CreateSelectionBuffers();
        void BuildPrismVertices();

        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };

        Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        HANDLE m_fenceEvent{ nullptr };
        std::uint64_t m_fenceValue{ 0 };

        Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[kBufferCount];
        D3D12_RESOURCE_STATES m_renderTargetStates[kBufferCount]{};
        std::uint32_t m_frameIndex{ 0 };

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        std::uint32_t m_rtvDescriptorSize{ 0 };
        Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_depthReadback;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_linePipelineState;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_axesPipelineState;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_markerPipelineState;

        // 世界坐标轴示意线：从中心出发的 ±X / ±Y / ±Z 共 6 条线
        static constexpr std::uint32_t kAxesVertexCapacity = 12;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_axesVertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_axesVertexBufferView{};
        PlyVertex* m_axesMappedVertices{ nullptr };
        std::uint32_t m_axesVertexCount{ 0 };
        bool m_axesVisible{ false };
        DirectX::XMFLOAT3 m_axesCenter{};
        float m_axesLength{ 0.0f };

        // 多边形选择体线框：前后两个多边形 + 连接边，顶点写在常驻映射的上传缓冲区里。
        static constexpr std::uint32_t kLineVertexCapacity = 256;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_lineVertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_lineVertexBufferView{};
        PlyVertex* m_lineMappedVertices{ nullptr };
        std::uint32_t m_lineVertexCount{ 0 };

        // 绘制过程中的多边形顶点标记（用小圆点显示，关闭深度测试，始终可见）
        Microsoft::WRL::ComPtr<ID3D12Resource> m_polygonVertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_polygonVertexBufferView{};
        PlyVertex* m_polygonMappedVertices{ nullptr };
        std::uint32_t m_polygonVertexCount{ 0 };

        Microsoft::WRL::ComPtr<ID3D12Resource> m_selectionConstantsBuffer;
        SelectionConstants* m_selectionMapped{ nullptr };

        bool m_prismVisible{ false };
        bool m_prismSelected{ false };
        bool m_clusterVisible{ false };
        DirectX::XMFLOAT3 m_clusterCenter{};
        float m_clusterRadius{ 0.0f };

        // 聚类选中的红色点（覆盖绘制）
        Microsoft::WRL::ComPtr<ID3D12Resource> m_clusterVertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_clusterVertexBufferView{};
        PlyVertex* m_clusterMappedVertices{ nullptr };
        std::uint32_t m_clusterVertexCapacity{ 0 };
        std::uint32_t m_clusterVertexCount{ 0 };
        SelectionFrame m_prismFrame{};
        std::vector<DirectX::XMFLOAT2> m_prismPolygon;
        float m_prismLength{ 1.0f };
        bool m_prismExtruded{ false };

        // 顶点数量不再设上限：每 kMaxVerticesPerBuffer 个点一个缓冲区，
        // 绘制时逐个 DrawInstanced，从而绕开 D3D12_VERTEX_BUFFER_VIEW.SizeInBytes 的 32 位限制。
        static constexpr std::size_t kMaxVerticesPerBuffer = 16'000'000; // 28B * 16M ≈ 448 MB
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_vertexBuffers;
        std::vector<D3D12_VERTEX_BUFFER_VIEW> m_vertexBufferViews;
        std::vector<std::uint32_t> m_vertexBufferCounts;
        std::size_t m_vertexCount{ 0 };

        std::uint32_t m_width{ 0 };
        std::uint32_t m_height{ 0 };
        std::wstring m_adapterName;
    };
}
