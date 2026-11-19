#include "pch.h"
#include "PointCloudRenderer.h"

// 显式包含，避免依赖预编译头的具体内容
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <microsoft.ui.xaml.media.dxinterop.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace app
{
namespace
{
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

    // 点精灵着色器：VS 变换顶点，GS 把每个点扩成屏幕对齐的四边形，PS 画成圆形。
    // 同一个文件里还有多边形选择体线框用的 VSLine / PSLine。
    // 注意：HLSL 的 const 必须写在类型前面（const float4 x），fxc 不接受 float4 const x。
    char const* const kSceneShader = R"hlsl(
cbuffer SceneConstants : register(b0)
{
    row_major float4x4 gViewProjection;
    float gPointSize;
    float gViewportWidth;
    float gViewportHeight;
    float gPadding;
    float gLineOffsetX;      // 线框加粗用的屏幕空间偏移（NDC）
    float gLineOffsetY;
    float gPadding2;
    float gPadding3;
};

cbuffer SelectionConstants : register(b1)
{
    float4 gSelOriginAndLength; // xyz = 多边形平面原点, w = 沿视线方向的长度
    float4 gSelRight;               // xyz = right, w = 是否可见
    float4 gSelUp;
    float4 gSelForward;
    float4 gPolygon[32];
    uint   gPolygonCount;
    uint   gSelPadding0;
    uint   gSelPadding1;
    uint   gSelPadding2;
    uint   gClusterEnabled;
    uint   gClusterPadding0;
    uint   gClusterPadding1;
    uint   gClusterPadding2;
    float4 gClusterCenterAndRadius; // xyz = 中心, w = 半径
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
};

struct VSOutput
{
    float4 clipPosition : SV_Position;
    float3 normal       : NORMAL;
    float4 color        : COLOR0;
};

struct PSInput
{
    float4 clipPosition : SV_Position;
    float4 color        : COLOR0;
    float2 local        : TEXCOORD0;
};

bool PointInsidePolygon(float2 p)
{
    bool inside = false;
    uint count = min(gPolygonCount, 32u);
    for (uint i = 0, j = count - 1; i < count; j = i++)
    {
        float2 a = gPolygon[j].xy;
        float2 b = gPolygon[i].xy;
        if (((b.y > p.y) != (a.y > p.y)) &&
            (p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x))
        {
            inside = !inside;
        }
    }
    return inside;
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.clipPosition = mul(float4(input.position, 1.0f), gViewProjection);
    output.normal = input.normal;
    output.color = input.color;

    // 多边形选择体（棱柱）内的点：显示为纯红，并去掉法线（跳过光照）
    if (gSelRight.w > 0.5f && gPolygonCount >= 3u)
    {
        float3 offset = input.position - gSelOriginAndLength.xyz;
        float2 local = float2(dot(offset, gSelRight.xyz), dot(offset, gSelUp.xyz));
        float depth = dot(offset, gSelForward.xyz);
        if (depth >= 0.0f && depth <= gSelOriginAndLength.w && PointInsidePolygon(local))
        {
            output.color = float4(1.0f, 0.05f, 0.05f, 1.0f);
            output.normal = float3(0.0f, 0.0f, 0.0f);
        }
    }

    // 点云聚类选择：球内的点显示为红色
    if (gClusterEnabled != 0u && gClusterCenterAndRadius.w > 0.0f)
    {
        if (distance(input.position, gClusterCenterAndRadius.xyz) <= gClusterCenterAndRadius.w)
        {
            output.color = float4(1.0f, 0.05f, 0.05f, 1.0f);
            output.normal = float3(0.0f, 0.0f, 0.0f);
        }
    }
    return output;
}

// 坐标轴线框
struct LineVSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
};

struct LinePSInput
{
    float4 clipPosition : SV_Position;
    float4 color        : COLOR0;
    float edge          : TEXCOORD0;
};

LinePSInput VSLine(LineVSInput input)
{
    LinePSInput output;
    output.clipPosition = mul(float4(input.position, 1.0f), gViewProjection);
    output.clipPosition.xy += float2(gLineOffsetX, gLineOffsetY) * output.clipPosition.w;
    output.color = input.color;
    output.edge = 0.0f;
    return output;
}

// 把线段扩展成屏幕空间的两个三角形（宽度按像素计），并带边缘属性用于抗锯齿。
[maxvertexcount(4)]
void GSLine(line LinePSInput input[2], inout TriangleStream<LinePSInput> stream)
{
    float4 c0 = input[0].clipPosition;
    float4 c1 = input[1].clipPosition;
    const float4 color = input[0].color;

    // 近平面裁剪：端点落在相机后面时先裁到近平面前面
    const float kEps = 1.0e-4f;
    if (c0.w < kEps && c1.w < kEps)
    {
        return;
    }
    if (c0.w < kEps)
    {
        c0 = lerp(c0, c1, (kEps - c0.w) / (c1.w - c0.w));
    }
    if (c1.w < kEps)
    {
        c1 = lerp(c1, c0, (kEps - c1.w) / (c0.w - c1.w));
    }

    const float2 ndc0 = c0.xy / c0.w;
    const float2 ndc1 = c1.xy / c1.w;
    float2 dir = ndc1 - ndc0;
    const float length2 = length(dir);
    if (length2 < 1.0e-7f)
    {
        return;
    }
    dir /= length2;
    const float2 normal = float2(-dir.y, dir.x);

    const float2 pixel = float2(2.0f / max(gViewportWidth, 1.0f), 2.0f / max(gViewportHeight, 1.0f));
    const float halfWidthPixels = 1.5f; // 约 3 像素宽的四边形，中间实心、两侧 1 像素渐变
    const float2 offset = normal * pixel * halfWidthPixels;

    LinePSInput vertex;
    vertex.color = color;

    vertex.clipPosition = c0 + float4(offset * c0.w, 0.0f, 0.0f);
    vertex.edge = 1.0f;
    stream.Append(vertex);

    vertex.clipPosition = c0 - float4(offset * c0.w, 0.0f, 0.0f);
    vertex.edge = -1.0f;
    stream.Append(vertex);

    vertex.clipPosition = c1 + float4(offset * c1.w, 0.0f, 0.0f);
    vertex.edge = 1.0f;
    stream.Append(vertex);

    vertex.clipPosition = c1 - float4(offset * c1.w, 0.0f, 0.0f);
    vertex.edge = -1.0f;
    stream.Append(vertex);
}

float4 PSLine(LinePSInput input) : SV_Target
{
    // 边缘做 1 像素左右的平滑过渡（配合 alpha 混合）
    const float dist = abs(input.edge);
    const float alpha = saturate(1.5f * (1.0f - dist));
    return float4(input.color.rgb, input.color.a * alpha);
}

[maxvertexcount(4)]
void GSMain(point VSOutput input[1], inout TriangleStream<PSInput> stream)
{
    const float4 clip = input[0].clipPosition;
    if (clip.w <= 1.0e-6f || gPointSize <= 0.0f)
    {
        return;
    }

    const float3 normal = input[0].normal;
    float3 color = input[0].color.rgb;
    if (dot(normal, normal) > 1.0e-8f)
    {
        const float3 lightDirection = normalize(float3(0.35f, 0.75f, 0.55f));
        const float shade = 0.35f + 0.65f * abs(dot(normalize(normal), lightDirection));
        color *= shade;
    }

    const float2 halfExtent =
        float2(gPointSize / gViewportWidth, gPointSize / gViewportHeight) * clip.w;

    PSInput output;
    output.color = float4(color, 1.0f);

    output.local = float2(-1.0f,  1.0f);
    output.clipPosition = float4(clip.xy + float2(-halfExtent.x,  halfExtent.y), clip.zw);
    stream.Append(output);

    output.local = float2( 1.0f,  1.0f);
    output.clipPosition = float4(clip.xy + float2( halfExtent.x,  halfExtent.y), clip.zw);
    stream.Append(output);

    output.local = float2(-1.0f, -1.0f);
    output.clipPosition = float4(clip.xy + float2(-halfExtent.x, -halfExtent.y), clip.zw);
    stream.Append(output);

    output.local = float2( 1.0f, -1.0f);
    output.clipPosition = float4(clip.xy + float2( halfExtent.x, -halfExtent.y), clip.zw);
    stream.Append(output);

    stream.RestartStrip();
}

float4 PSMain(PSInput input) : SV_Target
{
    const float radiusSquared = dot(input.local, input.local);
    if (radiusSquared > 1.0f)
    {
        discard;
    }
    const float rim = saturate((1.0f - radiusSquared) * 3.0f);
    return float4(input.color.rgb * (0.6f + 0.4f * rim), 1.0f);
}
)hlsl";

    void ThrowIfFailed(HRESULT result, char const* what)
    {
        if (FAILED(result))
        {
            char message[768];
            if (result == E_OUTOFMEMORY)
            {
                std::snprintf(
                    message,
                    sizeof(message),
                    "%s 失败：显存/内存不足（HRESULT 0x%08X），请尝试更小的点云或关闭其他程序。",
                    what,
                    static_cast<unsigned int>(result));
            }
            else
            {
                std::snprintf(message, sizeof(message), "%s 失败（HRESULT 0x%08X）。", what, static_cast<unsigned int>(result));
            }
            throw std::runtime_error(message);
        }
    }

    D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = type;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    D3D12_RESOURCE_DESC BufferDescription(std::uint64_t bytes)
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Alignment = 0;
        description.Width = bytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc = { 1, 0 };
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        return description;
    }

    D3D12_RESOURCE_BARRIER TransitionBarrier(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    ComPtr<ID3DBlob> CompileShader(char const* entryPoint, char const* target)
    {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        HRESULT const result = D3DCompile(
            kSceneShader,
            std::strlen(kSceneShader),
            "PointCloud.hlsl",
            nullptr,
            nullptr,
            entryPoint,
            target,
            flags,
            0,
            code.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(result))
        {
            std::string message = "着色器编译失败：";
            if (errors)
            {
                message.append(
                    static_cast<char const*>(errors->GetBufferPointer()),
                    static_cast<std::size_t>(errors->GetBufferSize()));
            }
            else
            {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08X", static_cast<unsigned int>(result));
                message.append(buffer);
            }
            throw std::runtime_error(message);
        }
        return code;
    }
} // namespace

PointCloudRenderer::~PointCloudRenderer()
{
    Shutdown();
}

void PointCloudRenderer::Initialize(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel)
{
    m_panel = panel;

    CreateDevice();
    CreateCommandObjects();
    CreatePipelineState();
    CreateSelectionBuffers();

    double scale = 1.0;
    if (auto const root = panel.XamlRoot())
    {
        scale = root.RasterizationScale();
    }

    if (panel.ActualWidth() > 0.0 && panel.ActualHeight() > 0.0)
    {
        m_width = static_cast<std::uint32_t>(std::max<long>(1, std::lround(panel.ActualWidth() * scale)));
        m_height = static_cast<std::uint32_t>(std::max<long>(1, std::lround(panel.ActualHeight() * scale)));
    }
    else
    {
        m_width = 1280;
        m_height = 720;
    }

    CreateSwapChain();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDescription.NumDescriptors = kBufferCount;
    rtvHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(m_rtvHeap.ReleaseAndGetAddressOf())), "创建 RTV 描述符堆");
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription{};
    dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDescription.NumDescriptors = 1;
    dsvHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDescription, IID_PPV_ARGS(m_dsvHeap.ReleaseAndGetAddressOf())), "创建 DSV 描述符堆");

    CreateRenderTargets();
    AttachSwapChain();
}

void PointCloudRenderer::CreateDevice()
{
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf())), "创建 DXGI 工厂");

    ComPtr<IDXGIAdapter1> bestAdapter;
    std::size_t bestVideoMemory = 0;

    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (m_factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
        {
            if (description.DedicatedVideoMemory > bestVideoMemory)
            {
                bestVideoMemory = description.DedicatedVideoMemory;
                bestAdapter = adapter;
                m_adapterName = description.Description;
            }
        }
    }

    if (bestAdapter)
    {
        ThrowIfFailed(
            D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_device.ReleaseAndGetAddressOf())),
            "创建 DirectX 12 设备");
    }
    else
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.ReleaseAndGetAddressOf())), "获取 WARP 适配器");
        ThrowIfFailed(
            D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_device.ReleaseAndGetAddressOf())),
            "创建 DirectX 12 WARP 设备");
        m_adapterName = L"WARP（软件渲染）";
    }
}

void PointCloudRenderer::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDescription.NodeMask = 0;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(m_commandQueue.ReleaseAndGetAddressOf())), "创建命令队列");

    ThrowIfFailed(
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_commandAllocator.ReleaseAndGetAddressOf())),
        "创建命令分配器");

    ThrowIfFailed(
        m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(m_commandList.ReleaseAndGetAddressOf())),
        "创建命令列表");
    ThrowIfFailed(m_commandList->Close(), "关闭初始命令列表");

    ThrowIfFailed(
        m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf())),
        "创建围栏");

    m_fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!m_fenceEvent)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "创建围栏事件");
    }
}

void PointCloudRenderer::CreatePipelineState()
{
    ComPtr<ID3DBlob> const vertexShader = CompileShader("VSMain", "vs_5_0");
    ComPtr<ID3DBlob> const geometryShader = CompileShader("GSMain", "gs_5_0");
    ComPtr<ID3DBlob> const pixelShader = CompileShader("PSMain", "ps_5_0");

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.RegisterSpace = 0;
    parameters[0].Constants.Num32BitValues = kRootConstantsCount;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].Descriptor.RegisterSpace = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};
    rootSignatureDescription.NumParameters = 2;
    rootSignatureDescription.pParameters = parameters;
    rootSignatureDescription.NumStaticSamplers = 0;
    rootSignatureDescription.pStaticSamplers = nullptr;
    rootSignatureDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> signatureErrors;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&rootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), signatureErrors.GetAddressOf()),
        "序列化根签名");

    ThrowIfFailed(
        m_device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
        "创建根签名");

    D3D12_INPUT_ELEMENT_DESC const inputElements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizer.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizer.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = FALSE;
    rasterizer.AntialiasedLineEnable = FALSE;
    rasterizer.ForcedSampleCount = 0;
    rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].LogicOpEnable = FALSE;
    blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC depthStencil{};
    depthStencil.DepthEnable = TRUE;
    depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depthStencil.StencilEnable = FALSE;
    depthStencil.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencil.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    depthStencil.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencil.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencil.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depthStencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencil.BackFace = depthStencil.FrontFace;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = m_rootSignature.Get();
    description.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    description.GS = { geometryShader->GetBufferPointer(), geometryShader->GetBufferSize() };
    description.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    description.DS = { nullptr, 0 };
    description.HS = { nullptr, 0 };
    description.StreamOutput = {};
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depthStencil;
    description.InputLayout = { inputElements, _countof(inputElements) };
    description.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = kBackBufferFormat;
    for (UINT index = 1; index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index)
    {
        description.RTVFormats[index] = DXGI_FORMAT_UNKNOWN;
    }
    description.DSVFormat = kDepthFormat;
    description.SampleDesc = { 1, 0 };
    description.NodeMask = 0;
    description.CachedPSO = { nullptr, 0 };
    description.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ThrowIfFailed(
        m_device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(m_pipelineState.ReleaseAndGetAddressOf())),
        "创建渲染管线状态对象");

    // 删除球线框：单独的 VS/PS，LINE 拓扑；关闭深度测试，保证球被点云挡住时也能看见。
    ComPtr<ID3DBlob> const lineVertexShader = CompileShader("VSLine", "vs_5_0");
    ComPtr<ID3DBlob> const linePixelShader = CompileShader("PSLine", "ps_5_0");

    D3D12_DEPTH_STENCIL_DESC lineDepth = depthStencil;
    lineDepth.DepthEnable = FALSE;
    lineDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lineDescription = description;
    lineDescription.VS = { lineVertexShader->GetBufferPointer(), lineVertexShader->GetBufferSize() };
    lineDescription.GS = { nullptr, 0 };
    lineDescription.PS = { linePixelShader->GetBufferPointer(), linePixelShader->GetBufferSize() };
    lineDescription.DepthStencilState = lineDepth;
    lineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;

    ThrowIfFailed(
        m_device->CreateGraphicsPipelineState(&lineDescription, IID_PPV_ARGS(m_linePipelineState.ReleaseAndGetAddressOf())),
        "创建删除球线框管线状态对象");

    // 坐标轴：线段经过 GS 扩展为屏幕空间四边形，开启 alpha 混合做边缘抗锯齿
    ComPtr<ID3DBlob> const lineGeometryShader = CompileShader("GSLine", "gs_5_0");

    D3D12_BLEND_DESC axesBlend = blend;
    axesBlend.RenderTarget[0].BlendEnable = TRUE;
    axesBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    axesBlend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    axesBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    axesBlend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    axesBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    axesBlend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC axesDescription = lineDescription;
    axesDescription.GS = { lineGeometryShader->GetBufferPointer(), lineGeometryShader->GetBufferSize() };
    axesDescription.BlendState = axesBlend;

    ThrowIfFailed(
        m_device->CreateGraphicsPipelineState(&axesDescription, IID_PPV_ARGS(m_axesPipelineState.ReleaseAndGetAddressOf())),
        "创建坐标轴管线状态对象");

    // 多边形顶点标记：点精灵管线，关闭深度测试，绘制过程中始终可见
    D3D12_DEPTH_STENCIL_DESC markerDepth = depthStencil;
    markerDepth.DepthEnable = FALSE;
    markerDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC markerDescription = description;
    markerDescription.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    markerDescription.GS = { geometryShader->GetBufferPointer(), geometryShader->GetBufferSize() };
    markerDescription.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    markerDescription.DepthStencilState = markerDepth;
    markerDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;

    ThrowIfFailed(
        m_device->CreateGraphicsPipelineState(&markerDescription, IID_PPV_ARGS(m_markerPipelineState.ReleaseAndGetAddressOf())),
        "创建多边形顶点标记管线状态对象");
}

void PointCloudRenderer::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = m_width;
    description.Height = m_height;
    description.Format = kBackBufferFormat;
    description.Stereo = FALSE;
    description.SampleDesc = { 1, 0 };
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kBufferCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = 0;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(
        m_factory->CreateSwapChainForComposition(m_commandQueue.Get(), &description, nullptr, swapChain.ReleaseAndGetAddressOf()),
        "创建合成交换链");

    ThrowIfFailed(
        swapChain->QueryInterface(IID_PPV_ARGS(m_swapChain.ReleaseAndGetAddressOf())),
        "获取 IDXGISwapChain3 接口");
    m_frameIndex = 0;
}

void PointCloudRenderer::AttachSwapChain()
{
    if (!m_panel || !m_swapChain)
    {
        return;
    }

    auto const panelNative = m_panel.as<::ISwapChainPanelNative>();
    ThrowIfFailed(panelNative->SetSwapChain(m_swapChain.Get()), "把交换链绑定到 SwapChainPanel");
}

void PointCloudRenderer::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < kBufferCount; ++index)
    {
        ThrowIfFailed(
            m_swapChain->GetBuffer(index, IID_PPV_ARGS(m_renderTargets[index].ReleaseAndGetAddressOf())),
            "获取交换链缓冲区");

        m_device->CreateRenderTargetView(m_renderTargets[index].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
        m_renderTargetStates[index] = D3D12_RESOURCE_STATE_COMMON;
    }

    D3D12_RESOURCE_DESC depthDescription{};
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Alignment = 0;
    depthDescription.Width = m_width;
    depthDescription.Height = m_height;
    depthDescription.DepthOrArraySize = 1;
    depthDescription.MipLevels = 1;
    depthDescription.Format = kDepthFormat;
    depthDescription.SampleDesc = { 1, 0 };
    depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kDepthFormat;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES const heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(
        m_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &depthDescription,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(m_depthBuffer.ReleaseAndGetAddressOf())),
        "创建深度缓冲区");

    D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
    depthView.Format = kDepthFormat;
    depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depthView.Flags = D3D12_DSV_FLAG_NONE;
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), &depthView, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    m_frameIndex = 0;
}

void PointCloudRenderer::ReleaseRenderTargets()
{
    for (auto& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
    m_depthBuffer.Reset();
}

void PointCloudRenderer::CreateSelectionBuffers()
{
    D3D12_HEAP_PROPERTIES const heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RANGE const readRange{ 0, 0 };
    void* mapped = nullptr;

    // 多边形线框顶点缓冲（前后两个多边形 + 连接边）
    D3D12_RESOURCE_DESC const lineDescription =
        BufferDescription(static_cast<std::uint64_t>(kLineVertexCapacity) * sizeof(PlyVertex));
    ThrowIfFailed(
        m_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &lineDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(m_lineVertexBuffer.ReleaseAndGetAddressOf())),
        "创建多边形线框缓冲区");

    ThrowIfFailed(m_lineVertexBuffer->Map(0, &readRange, &mapped), "映射多边形线框缓冲区");
    m_lineMappedVertices = static_cast<PlyVertex*>(mapped);

    m_lineVertexBufferView.BufferLocation = m_lineVertexBuffer->GetGPUVirtualAddress();
    m_lineVertexBufferView.StrideInBytes = sizeof(PlyVertex);
    m_lineVertexBufferView.SizeInBytes = static_cast<UINT>(kLineVertexCapacity * sizeof(PlyVertex));

    // 世界坐标轴示意线（±X / ±Y / ±Z 共 6 条线、12 个顶点）
    D3D12_RESOURCE_DESC const axesDescription =
        BufferDescription(static_cast<std::uint64_t>(kAxesVertexCapacity) * sizeof(PlyVertex));
    ThrowIfFailed(
        m_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &axesDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(m_axesVertexBuffer.ReleaseAndGetAddressOf())),
        "创建坐标轴缓冲区");

    void* axesMapped = nullptr;
    ThrowIfFailed(m_axesVertexBuffer->Map(0, &readRange, &axesMapped), "映射坐标轴缓冲区");
    m_axesMappedVertices = static_cast<PlyVertex*>(axesMapped);

    m_axesVertexBufferView.BufferLocation = m_axesVertexBuffer->GetGPUVirtualAddress();
    m_axesVertexBufferView.StrideInBytes = sizeof(PlyVertex);
    m_axesVertexBufferView.SizeInBytes = static_cast<UINT>(kAxesVertexCapacity * sizeof(PlyVertex));

    // 选择体常量缓冲（根 CBV b1）。常量缓冲区按 256 字节对齐，这里留出余量。
    D3D12_RESOURCE_DESC const constantsDescription = BufferDescription(1024);
    ThrowIfFailed(
        m_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &constantsDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(m_selectionConstantsBuffer.ReleaseAndGetAddressOf())),
        "创建多边形选择常量缓冲区");

    mapped = nullptr;
    ThrowIfFailed(m_selectionConstantsBuffer->Map(0, &readRange, &mapped), "映射多边形选择常量缓冲区");
    m_selectionMapped = static_cast<SelectionConstants*>(mapped);
    std::memset(m_selectionMapped, 0, sizeof(SelectionConstants));

    // 绘制过程中的多边形顶点标记
    D3D12_RESOURCE_DESC const markerDescription =
        BufferDescription(static_cast<std::uint64_t>(kMaxPolygonVertices) * sizeof(PlyVertex));
    ThrowIfFailed(
        m_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &markerDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(m_polygonVertexBuffer.ReleaseAndGetAddressOf())),
        "创建多边形顶点标记缓冲区");

    mapped = nullptr;
    ThrowIfFailed(m_polygonVertexBuffer->Map(0, &readRange, &mapped), "映射多边形顶点标记缓冲区");
    m_polygonMappedVertices = static_cast<PlyVertex*>(mapped);

    m_polygonVertexBufferView.BufferLocation = m_polygonVertexBuffer->GetGPUVirtualAddress();
    m_polygonVertexBufferView.StrideInBytes = sizeof(PlyVertex);
    m_polygonVertexBufferView.SizeInBytes = static_cast<UINT>(kMaxPolygonVertices * sizeof(PlyVertex));
}

void PointCloudRenderer::BuildPrismVertices()
{
    // 聚类选择的常量与多边形无关，先写入（即使没有多边形也要生效）
    if (m_selectionMapped)
    {
        m_selectionMapped->clusterEnabled = m_clusterVisible ? 1u : 0u;
        m_selectionMapped->clusterCenterAndRadius = {
            m_clusterCenter.x, m_clusterCenter.y, m_clusterCenter.z, m_clusterRadius };
    }

    if (!m_lineMappedVertices)
    {
        return;
    }

    m_lineVertexCount = 0;
    m_polygonVertexCount = 0;
    std::size_t const count = m_prismPolygon.size();
    if (count < 1)
    {
        return;
    }

    // 橙色线框（RGBA8：R | G<<8 | B<<16 | A<<24）
    std::uint32_t const color = 0xFF26B5FF;
    auto const& frame = m_prismFrame;

    auto addVertex = [&](float x, float y, float depth)
    {
        if (m_lineVertexCount >= kLineVertexCapacity)
        {
            return;
        }
        PlyVertex vertex;
        vertex.position = {
            frame.origin.x + frame.right.x * x + frame.up.x * y + frame.forward.x * depth,
            frame.origin.y + frame.right.y * x + frame.up.y * y + frame.forward.y * depth,
            frame.origin.z + frame.right.z * x + frame.up.z * y + frame.forward.z * depth };
        vertex.color = color;
        m_lineMappedVertices[m_lineVertexCount++] = vertex;
    };

    auto addSegment = [&](float x0, float y0, float d0, float x1, float y1, float d1)
    {
        addVertex(x0, y0, d0);
        addVertex(x1, y1, d1);
    };

    if (m_prismExtruded && count >= 3)
    {
        // 已确认：闭合的前后两个多边形 + 连接边
        float const length = m_prismLength;
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t const next = (i + 1) % count;
            addSegment(
                m_prismPolygon[i].x, m_prismPolygon[i].y, 0.0f,
                m_prismPolygon[next].x, m_prismPolygon[next].y, 0.0f);
            addSegment(
                m_prismPolygon[i].x, m_prismPolygon[i].y, length,
                m_prismPolygon[next].x, m_prismPolygon[next].y, length);
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            addSegment(
                m_prismPolygon[i].x, m_prismPolygon[i].y, 0.0f,
                m_prismPolygon[i].x, m_prismPolygon[i].y, length);
        }
    }
    else if (!m_prismExtruded)
    {
        // 绘制中：只画开放折线（不闭合），并显示每个顶点的小圆点
        for (std::size_t i = 0; i + 1 < count; ++i)
        {
            addSegment(
                m_prismPolygon[i].x, m_prismPolygon[i].y, 0.0f,
                m_prismPolygon[i + 1].x, m_prismPolygon[i + 1].y, 0.0f);
        }

        if (m_polygonMappedVertices)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                PlyVertex vertex;
                vertex.position = {
                    frame.origin.x + frame.right.x * m_prismPolygon[i].x + frame.up.x * m_prismPolygon[i].y,
                    frame.origin.y + frame.right.y * m_prismPolygon[i].x + frame.up.y * m_prismPolygon[i].y,
                    frame.origin.z + frame.right.z * m_prismPolygon[i].x + frame.up.z * m_prismPolygon[i].y };
                vertex.color = 0xFF3CE0FF; // 亮黄色标记
                m_polygonMappedVertices[m_polygonVertexCount++] = vertex;
            }
        }
    }

    if (m_selectionMapped)
    {
        m_selectionMapped->originAndLength = {
            frame.origin.x, frame.origin.y, frame.origin.z, m_prismLength };
        // 只有确认后（≥3 个点）才做红色高亮判定
        m_selectionMapped->right = {
            frame.right.x, frame.right.y, frame.right.z,
            (m_prismVisible && m_prismExtruded && count >= 3) ? 1.0f : 0.0f };
        m_selectionMapped->up = { frame.up.x, frame.up.y, frame.up.z, 0.0f };
        m_selectionMapped->forward = { frame.forward.x, frame.forward.y, frame.forward.z, 0.0f };
        m_selectionMapped->polygonCount = static_cast<std::uint32_t>(std::min<std::size_t>(
            count, kMaxPolygonVertices));
        for (std::uint32_t i = 0; i < m_selectionMapped->polygonCount; ++i)
        {
            m_selectionMapped->polygon[i] = {
                m_prismPolygon[i].x, m_prismPolygon[i].y, 0.0f, 0.0f };
        }
    }
}

void PointCloudRenderer::SetSelectionPrism(
    bool visible,
    SelectionFrame const& frame,
    std::vector<DirectX::XMFLOAT2> const& polygon,
    float length,
    bool extruded,
    bool selected)
{
    m_prismVisible = visible && !polygon.empty();
    m_prismSelected = m_prismVisible && selected;
    m_prismFrame = frame;
    m_prismPolygon.assign(polygon.begin(), polygon.begin() + std::min<std::size_t>(
        polygon.size(), kMaxPolygonVertices));
    m_prismLength = std::max(length, 0.0f);
    m_prismExtruded = extruded;

    if (m_selectionMapped)
    {
        m_selectionMapped->right.w = m_prismVisible ? 1.0f : 0.0f;
        if (!m_prismVisible)
        {
            m_selectionMapped->polygonCount = 0;
        }
    }
    BuildPrismVertices();
}

void PointCloudRenderer::SetClusterSelection(bool visible, DirectX::XMFLOAT3 const& center, float radius)
{
    m_clusterVisible = visible && radius > 0.0f;
    m_clusterCenter = center;
    m_clusterRadius = std::max(radius, 0.0f);
    BuildPrismVertices();
}

void PointCloudRenderer::SetAxes(DirectX::XMFLOAT3 const& center, float length, bool visible)
{
    if (!m_axesMappedVertices)
    {
        return;
    }

    if (!visible || !(length > 0.0f))
    {
        m_axesVisible = false;
        m_axesVertexCount = 0;
        return;
    }

    // 中心/长度不变时无需重写（每帧调用也只是空操作）
    if (m_axesVisible &&
        m_axesVertexCount == kAxesVertexCapacity &&
        center.x == m_axesCenter.x && center.y == m_axesCenter.y && center.z == m_axesCenter.z &&
        length == m_axesLength)
    {
        return;
    }

    m_axesVisible = true;
    m_axesCenter = center;
    m_axesLength = length;
    m_axesVertexCount = 0;

    auto pushVertex = [this](float x, float y, float z, std::uint32_t color)
    {
        PlyVertex& vertex = m_axesMappedVertices[m_axesVertexCount++];
        vertex.position = { x, y, z };
        vertex.normal = { 0.0f, 0.0f, 0.0f };
        vertex.color = color;
    };

    auto pushAxis = [&](float dx, float dy, float dz, std::uint32_t positiveColor, std::uint32_t negativeColor)
    {
        // 正负两个方向都延伸到“无限远”（由近/远平面裁剪）
        pushVertex(center.x, center.y, center.z, positiveColor);
        pushVertex(
            center.x + dx * length,
            center.y + dy * length,
            center.z + dz * length,
            positiveColor);

        pushVertex(center.x, center.y, center.z, negativeColor);
        pushVertex(
            center.x - dx * length,
            center.y - dy * length,
            center.z - dz * length,
            negativeColor);
    };

    // 颜色格式：R | G << 8 | B << 16 | A << 24；负轴用更浅的同色系
    pushAxis(1.0f, 0.0f, 0.0f, 0xFF3C3CFFu, 0xFFA0A0FFu); // X 红：正轴鲜红，负轴浅红
    pushAxis(0.0f, 1.0f, 0.0f, 0xFF50DC3Cu, 0xFFA0EB96u); // Y 绿：正轴鲜绿，负轴浅绿
    pushAxis(0.0f, 0.0f, 1.0f, 0xFFFF8C46u, 0xFFFFC8A0u); // Z 蓝：正轴鲜蓝，负轴浅蓝
}

void PointCloudRenderer::SetClusterPoints(std::vector<PlyVertex> const& points)
{
    m_clusterVertexCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(points.size(), 4'000'000));
    if (m_clusterVertexCount == 0)
    {
        return;
    }

    if (!m_clusterVertexBuffer || m_clusterVertexCapacity < m_clusterVertexCount)
    {
        m_clusterVertexBuffer.Reset();
        m_clusterMappedVertices = nullptr;

        std::uint32_t capacity = 65'536;
        while (capacity < m_clusterVertexCount)
        {
            capacity *= 2;
        }

        D3D12_HEAP_PROPERTIES const heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC const description =
            BufferDescription(static_cast<std::uint64_t>(capacity) * sizeof(PlyVertex));
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(m_clusterVertexBuffer.ReleaseAndGetAddressOf())),
            "创建聚类选中点缓冲区");

        D3D12_RANGE const readRange{ 0, 0 };
        void* mapped = nullptr;
        ThrowIfFailed(m_clusterVertexBuffer->Map(0, &readRange, &mapped), "映射聚类选中点缓冲区");
        m_clusterMappedVertices = static_cast<PlyVertex*>(mapped);
        m_clusterVertexCapacity = capacity;

        m_clusterVertexBufferView.BufferLocation = m_clusterVertexBuffer->GetGPUVirtualAddress();
        m_clusterVertexBufferView.StrideInBytes = sizeof(PlyVertex);
        m_clusterVertexBufferView.SizeInBytes = static_cast<UINT>(capacity * sizeof(PlyVertex));
    }

    std::memcpy(
        m_clusterMappedVertices,
        points.data(),
        static_cast<std::size_t>(m_clusterVertexCount) * sizeof(PlyVertex));
}

void PointCloudRenderer::Resize(std::uint32_t pixelWidth, std::uint32_t pixelHeight)
{
    if (!m_swapChain)
    {
        return;
    }

    pixelWidth = std::max<std::uint32_t>(pixelWidth, 1);
    pixelHeight = std::max<std::uint32_t>(pixelHeight, 1);
    if (pixelWidth == m_width && pixelHeight == m_height)
    {
        return;
    }

    WaitForGpu();
    ReleaseRenderTargets();

    m_width = pixelWidth;
    m_height = pixelHeight;

    ThrowIfFailed(
        m_swapChain->ResizeBuffers(kBufferCount, m_width, m_height, kBackBufferFormat, 0),
        "调整交换链缓冲区");

    CreateRenderTargets();
}

void PointCloudRenderer::SetPoints(std::vector<PlyVertex> const& vertices)
{
    WaitForGpu();

    m_vertexCount = vertices.size();
    m_vertexBuffers.clear();
    m_vertexBufferViews.clear();
    m_vertexBufferCounts.clear();
    m_vertexBuffers.shrink_to_fit();
    m_vertexBufferViews.shrink_to_fit();
    m_vertexBufferCounts.shrink_to_fit();

    if (vertices.empty())
    {
        return;
    }

    // 点数不设上限：按 kMaxVerticesPerBuffer 切成多个顶点缓冲区，
    // 这样每个缓冲区的 SizeInBytes 都不会超过 32 位的 D3D12_VERTEX_BUFFER_VIEW 限制。
    std::size_t const totalVertices = vertices.size();
    std::size_t const chunkCount =
        (totalVertices + kMaxVerticesPerBuffer - 1) / kMaxVerticesPerBuffer;
    m_vertexBuffers.reserve(chunkCount);
    m_vertexBufferViews.reserve(chunkCount);
    m_vertexBufferCounts.reserve(chunkCount);

    D3D12_HEAP_PROPERTIES const defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES const uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);

    std::vector<ComPtr<ID3D12Resource>> uploadBuffers;
    uploadBuffers.reserve(chunkCount);

    for (std::size_t offset = 0; offset < totalVertices; offset += kMaxVerticesPerBuffer)
    {
        std::size_t const count = std::min(kMaxVerticesPerBuffer, totalVertices - offset);
        std::uint64_t const bytes = static_cast<std::uint64_t>(count) * sizeof(PlyVertex);
        D3D12_RESOURCE_DESC const bufferDescription = BufferDescription(bytes);

        ComPtr<ID3D12Resource> vertexBuffer;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(vertexBuffer.ReleaseAndGetAddressOf())),
            "创建顶点缓冲区");

        ComPtr<ID3D12Resource> uploadBuffer;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &bufferDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(uploadBuffer.ReleaseAndGetAddressOf())),
            "创建上传缓冲区");

        void* mapped = nullptr;
        D3D12_RANGE const readRange{ 0, 0 };
        ThrowIfFailed(uploadBuffer->Map(0, &readRange, &mapped), "映射上传缓冲区");
        std::memcpy(mapped, vertices.data() + offset, static_cast<std::size_t>(bytes));
        uploadBuffer->Unmap(0, nullptr);

        D3D12_VERTEX_BUFFER_VIEW view{};
        view.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        view.StrideInBytes = sizeof(PlyVertex);
        view.SizeInBytes = static_cast<UINT>(bytes);

        m_vertexBuffers.push_back(std::move(vertexBuffer));
        m_vertexBufferViews.push_back(view);
        m_vertexBufferCounts.push_back(static_cast<std::uint32_t>(count));
        uploadBuffers.push_back(std::move(uploadBuffer));
    }

    ThrowIfFailed(m_commandAllocator->Reset(), "重置命令分配器");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr), "重置命令列表");

    for (std::size_t index = 0; index < m_vertexBuffers.size(); ++index)
    {
        std::uint64_t const bytes =
            static_cast<std::uint64_t>(m_vertexBufferCounts[index]) * sizeof(PlyVertex);
        m_commandList->CopyBufferRegion(
            m_vertexBuffers[index].Get(), 0, uploadBuffers[index].Get(), 0, bytes);

        D3D12_RESOURCE_BARRIER const barrier = TransitionBarrier(
            m_vertexBuffers[index].Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        m_commandList->ResourceBarrier(1, &barrier);
    }

    ThrowIfFailed(m_commandList->Close(), "关闭命令列表");

    ID3D12CommandList* const commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForGpu();
    // uploadBuffers 在这里析构，GPU 已经执行完毕，可以安全释放。
}

void PointCloudRenderer::Render(DirectX::XMFLOAT4X4 const& viewProjection, float pointSizePixels)
{
    if (!IsReady())
    {
        return;
    }

    WaitForGpu();

    ThrowIfFailed(m_commandAllocator->Reset(), "重置命令分配器");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()), "重置命令列表");

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE const dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_BARRIER const toRenderTarget = TransitionBarrier(
        m_renderTargets[m_frameIndex].Get(),
        m_renderTargetStates[m_frameIndex],
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &toRenderTarget);

    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    m_commandList->ClearRenderTargetView(rtvHandle, m_clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(m_width);
    scissor.bottom = static_cast<LONG>(m_height);

    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    bool const drawPoints = m_vertexCount > 0 && !m_vertexBuffers.empty();
    bool const drawLines = m_prismVisible && m_lineVertexCount > 0 && m_lineVertexBuffer;
    bool const drawMarkers =
        m_prismVisible && !m_prismExtruded && m_polygonVertexCount > 0 && m_polygonVertexBuffer;
    bool const drawCluster = m_clusterVertexCount > 0 && m_clusterVertexBuffer;

    SceneConstants constants{};
    if (drawPoints || drawLines || drawMarkers || drawCluster)
    {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

        constants.viewProjection = viewProjection;
        constants.pointSize = pointSizePixels;
        constants.viewportWidth = static_cast<float>(m_width);
        constants.viewportHeight = static_cast<float>(m_height);
        constants.padding = 0.0f;
        constants.lineOffsetX = 0.0f;
        constants.lineOffsetY = 0.0f;
        constants.padding2 = 0.0f;
        constants.padding3 = 0.0f;
        m_commandList->SetGraphicsRoot32BitConstants(0, kRootConstantsCount, &constants, 0);

        if (m_selectionConstantsBuffer)
        {
            m_commandList->SetGraphicsRootConstantBufferView(
                1, m_selectionConstantsBuffer->GetGPUVirtualAddress());
        }
    }

    if (drawPoints)
    {
        m_commandList->SetPipelineState(m_pipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

        // 分块绘制：每块一个顶点缓冲区（见 SetPoints）
        for (std::size_t index = 0; index < m_vertexBuffers.size(); ++index)
        {
            m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferViews[index]);
            m_commandList->DrawInstanced(m_vertexBufferCounts[index], 1, 0, 0);
        }
    }

    if (drawCluster)
    {
        // 聚类选中的红色点：位置与主点云相同，深度 LESS_EQUAL 保证覆盖可见
        m_commandList->SetPipelineState(m_pipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_clusterVertexBufferView);
        m_commandList->DrawInstanced(m_clusterVertexCount, 1, 0, 0);
    }

    if (drawLines)
    {
        m_commandList->SetPipelineState(m_linePipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_lineVertexBufferView);

        // 选中时用多个微小偏移画几遍，等效加粗线框
        constexpr float kHalfStepPixels = 0.75f;
        float const stepX = 2.0f * kHalfStepPixels / std::max(1.0f, static_cast<float>(m_width));
        float const stepY = 2.0f * kHalfStepPixels / std::max(1.0f, static_cast<float>(m_height));

        int const passes = m_prismSelected ? 4 : 1;
        for (int pass = 0; pass < passes; ++pass)
        {
            constants.lineOffsetX = (pass & 1) ? stepX : -stepX;
            constants.lineOffsetY = (pass & 2) ? stepY : -stepY;
            if (!m_prismSelected)
            {
                constants.lineOffsetX = 0.0f;
                constants.lineOffsetY = 0.0f;
            }
            m_commandList->SetGraphicsRoot32BitConstants(0, kRootConstantsCount, &constants, 0);
            m_commandList->DrawInstanced(m_lineVertexCount, 1, 0, 0);
        }
    }

    if (drawMarkers)
    {
        // 顶点标记用固定的 8 像素点，关闭深度测试，始终可见
        SceneConstants markerConstants = constants;
        markerConstants.pointSize = 8.0f;
        m_commandList->SetGraphicsRoot32BitConstants(0, kRootConstantsCount, &markerConstants, 0);

        m_commandList->SetPipelineState(m_markerPipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_polygonVertexBufferView);
        m_commandList->DrawInstanced(m_polygonVertexCount, 1, 0, 0);
    }

    if (m_axesVisible && m_axesVertexCount > 0 && m_axesVertexBuffer && m_axesPipelineState)
    {
        m_commandList->SetPipelineState(m_axesPipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_axesVertexBufferView);
        m_commandList->SetGraphicsRoot32BitConstants(0, kRootConstantsCount, &constants, 0);
        m_commandList->DrawInstanced(m_axesVertexCount, 1, 0, 0);
    }

    D3D12_RESOURCE_BARRIER const toPresent = TransitionBarrier(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &toPresent);
    m_renderTargetStates[m_frameIndex] = D3D12_RESOURCE_STATE_PRESENT;

    ThrowIfFailed(m_commandList->Close(), "关闭命令列表");

    ID3D12CommandList* const commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    HRESULT const presentResult = m_swapChain->Present(1, 0);
    if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET)
    {
        char message[256];
        std::snprintf(
            message,
            sizeof(message),
            "DirectX 12 设备丢失（HRESULT 0x%08X，原因 0x%08X）。",
            static_cast<unsigned int>(presentResult),
            static_cast<unsigned int>(m_device->GetDeviceRemovedReason()));
        throw std::runtime_error(message);
    }
    ThrowIfFailed(presentResult, "呈现画面");

    m_frameIndex = (m_frameIndex + 1) % kBufferCount;

    ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue), "提交围栏信号");
}

bool PointCloudRenderer::PickDepth(std::uint32_t x, std::uint32_t y, float& ndcDepth)
{
    if (!IsReady() || !m_depthBuffer || !m_commandList || !m_commandAllocator)
    {
        return false;
    }
    if (x >= m_width || y >= m_height)
    {
        return false;
    }

    WaitForGpu();

    if (!m_depthReadback)
    {
        D3D12_HEAP_PROPERTIES const heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC const description = BufferDescription(512);
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(m_depthReadback.ReleaseAndGetAddressOf())),
            "创建深度读回缓冲区");
    }

    ThrowIfFailed(m_commandAllocator->Reset(), "重置命令分配器（深度读取）");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr), "重置命令列表（深度读取）");

    D3D12_RESOURCE_BARRIER const toCopySource = TransitionBarrier(
        m_depthBuffer.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->ResourceBarrier(1, &toCopySource);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_depthReadback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format = kDepthFormat;
    destination.PlacedFootprint.Footprint.Width = 1;
    destination.PlacedFootprint.Footprint.Height = 1;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = 256;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_depthBuffer.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;

    D3D12_BOX const box{ x, y, 0, x + 1, y + 1, 1 };
    m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);

    D3D12_RESOURCE_BARRIER const toDepthWrite = TransitionBarrier(
        m_depthBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    m_commandList->ResourceBarrier(1, &toDepthWrite);

    ThrowIfFailed(m_commandList->Close(), "关闭命令列表（深度读取）");
    ID3D12CommandList* const lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();

    void* mapped = nullptr;
    D3D12_RANGE const readRange{ 0, sizeof(float) };
    ThrowIfFailed(m_depthReadback->Map(0, &readRange, &mapped), "映射深度读回缓冲区");
    ndcDepth = *static_cast<float*>(mapped);
    D3D12_RANGE const writeRange{ 0, 0 };
    m_depthReadback->Unmap(0, &writeRange);

    return std::isfinite(ndcDepth) && ndcDepth < 1.0f;
}

void PointCloudRenderer::WaitForGpu()
{
    if (!m_commandQueue || !m_fence || !m_fenceEvent)
    {
        return;
    }

    ++m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue), "提交围栏信号");

    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent), "设置围栏事件");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void PointCloudRenderer::Shutdown()
{
    if (m_commandQueue && m_fence && m_fenceEvent)
    {
        try
        {
            WaitForGpu();
        }
        catch (...)
        {
        }
    }

    if (m_panel && m_swapChain)
    {
        try
        {
            auto const panelNative = m_panel.as<::ISwapChainPanelNative>();
            panelNative->SetSwapChain(nullptr);
        }
        catch (...)
        {
        }
    }

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    m_vertexBuffers.clear();
    m_vertexBufferViews.clear();
    m_vertexBufferCounts.clear();
    m_vertexCount = 0;

    if (m_lineVertexBuffer && m_lineMappedVertices)
    {
        m_lineVertexBuffer->Unmap(0, nullptr);
        m_lineMappedVertices = nullptr;
    }
    m_lineVertexBuffer.Reset();
    m_lineVertexBufferView = {};
    m_lineVertexCount = 0;

    if (m_axesVertexBuffer && m_axesMappedVertices)
    {
        m_axesVertexBuffer->Unmap(0, nullptr);
        m_axesMappedVertices = nullptr;
    }
    m_axesVertexBuffer.Reset();
    m_axesVertexBufferView = {};
    m_axesVertexCount = 0;
    m_axesVisible = false;

    if (m_selectionConstantsBuffer && m_selectionMapped)
    {
        m_selectionConstantsBuffer->Unmap(0, nullptr);
        m_selectionMapped = nullptr;
    }
    m_selectionConstantsBuffer.Reset();

    if (m_polygonVertexBuffer && m_polygonMappedVertices)
    {
        m_polygonVertexBuffer->Unmap(0, nullptr);
        m_polygonMappedVertices = nullptr;
    }
    m_polygonVertexBuffer.Reset();
    m_polygonVertexBufferView = {};
    m_polygonVertexCount = 0;

    m_prismVisible = false;
    m_prismPolygon.clear();

    if (m_clusterVertexBuffer && m_clusterMappedVertices)
    {
        m_clusterVertexBuffer->Unmap(0, nullptr);
        m_clusterMappedVertices = nullptr;
    }
    m_clusterVertexBuffer.Reset();
    m_clusterVertexBufferView = {};
    m_clusterVertexCapacity = 0;
    m_clusterVertexCount = 0;

    m_markerPipelineState.Reset();
    m_linePipelineState.Reset();
    m_axesPipelineState.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_dsvHeap.Reset();
    m_depthBuffer.Reset();
    m_depthReadback.Reset();
    m_rtvHeap.Reset();
    for (auto& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
    m_swapChain.Reset();
    m_fence.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    m_factory.Reset();
    m_panel = nullptr;

    m_width = 0;
    m_height = 0;
}

} // namespace app
