#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace app
{
    // 与输入布局一致的顶点格式：位置 (12B) + 法线 (12B) + RGBA8 颜色 (4B)
    struct PlyVertex
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 normal{};
        std::uint32_t color{ 0xFFFFFFFFu }; // R | G << 8 | B << 16 | A << 24
    };

    static_assert(sizeof(PlyVertex) == 28, "PlyVertex 必须与 D3D12 输入布局匹配");

    struct PlyCloud
    {
        std::vector<PlyVertex> vertices;
        DirectX::XMFLOAT3 boundsMin{};
        DirectX::XMFLOAT3 boundsMax{};
        std::uint64_t declaredVertexCount{ 0 };
        std::uint64_t loadedVertexCount{ 0 };
        bool hasColors{ false };
        bool hasNormals{ false };
        std::string format; // ascii / binary_little_endian / binary_big_endian
        std::uint64_t fileSizeBytes{ 0 };
    };

    // 读取 PLY 点云文件。支持 ascii、binary_little_endian、binary_big_endian，
    // 顶点属性支持 x/y/z、nx/ny/nz、red/green/blue（或 diffuse_*）、intensity。
    // 失败时抛出 std::runtime_error，消息为 UTF-8。
    PlyCloud LoadPlyFile(std::wstring const& path);

    // 读取 PCD 点云文件（PCL 格式）：支持 ascii / binary / binary_compressed；
    // 字段支持 x/y/z、normal_x/y/z、rgb/rgba、intensity。
    // 失败时抛出 std::runtime_error，消息为 UTF-8。
    PlyCloud LoadPcdFile(std::wstring const& path);

    // 按扩展名分派：.pcd → LoadPcdFile，其余 → LoadPlyFile。
    PlyCloud LoadPointCloudFile(std::wstring const& path);

    // 把点云保存为 binary_little_endian PLY（x/y/z、nx/ny/nz、red/green/blue）。
    // 失败时抛出 std::runtime_error，消息为 UTF-8。
    void SavePlyFile(std::wstring const& path, std::vector<PlyVertex> const& vertices);

    // 把点云保存为 binary PCD（x/y/z、rgb、normal_x/normal_y/normal_z）。
    // 失败时抛出 std::runtime_error，消息为 UTF-8。
    void SavePcdFile(std::wstring const& path, std::vector<PlyVertex> const& vertices);

    // 生成内置示例点云（彩色环面），用于无需外部文件即可体验渲染与交互。
    PlyCloud MakeDemoPointCloud();
}
