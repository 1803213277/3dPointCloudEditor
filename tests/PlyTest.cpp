// 独立的 PLY/PCD 解析 + 相机取景自检程序（不属于应用本身，仅用于验证）。
// 编译并运行：见 tests/build-test.ps1
// 生成的临时 PCD/PLY 默认写到 %TEMP%\3dPointCloudEditor-tests，可用 --out-dir 指定。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "PlyLoader.h"
#include "OrbitCamera.h"

namespace
{
    int g_failures = 0;

    void Check(bool condition, char const* what)
    {
        if (!condition)
        {
            std::printf("  [FAIL] %s\n", what);
            ++g_failures;
        }
        else
        {
            std::printf("  [ ok ] %s\n", what);
        }
    }

    void Dump(char const* label, app::PlyCloud const& cloud)
    {
        std::printf("%s: format=%s points=%llu/%llu colors=%d normals=%d\n",
                    label,
                    cloud.format.c_str(),
                    static_cast<unsigned long long>(cloud.loadedVertexCount),
                    static_cast<unsigned long long>(cloud.declaredVertexCount),
                    cloud.hasColors ? 1 : 0,
                    cloud.hasNormals ? 1 : 0);
        std::printf("  bounds = (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n",
                    cloud.boundsMin.x, cloud.boundsMin.y, cloud.boundsMin.z,
                    cloud.boundsMax.x, cloud.boundsMax.y, cloud.boundsMax.z);
    }

    // 与渲染器相同的矩阵约定：row_major，mul(rowVec, viewProjection)
    bool ProjectToNdc(
        DirectX::XMFLOAT4X4 const& viewProjection,
        DirectX::XMFLOAT3 const& point,
        DirectX::XMFLOAT3& ndc,
        float& w)
    {
        DirectX::XMVECTOR const p = DirectX::XMVectorSet(point.x, point.y, point.z, 1.0f);
        DirectX::XMVECTOR const clip = DirectX::XMVector4Transform(p, DirectX::XMLoadFloat4x4(&viewProjection));
        w = DirectX::XMVectorGetW(clip);
        if (w <= 1.0e-6f) return false;
        ndc = {
            DirectX::XMVectorGetX(clip) / w,
            DirectX::XMVectorGetY(clip) / w,
            DirectX::XMVectorGetZ(clip) / w };
        return true;
    }

    void WritePcdHeader(std::ofstream& file, char const* dataMode)
    {
        file << "# .PCD v0.7 - Point Cloud Data file format\n"
             << "VERSION 0.7\n"
             << "FIELDS x y z rgb\n"
             << "SIZE 4 4 4 4\n"
             << "TYPE F F F F\n"
             << "COUNT 1 1 1 1\n"
             << "WIDTH 3\n"
             << "HEIGHT 1\n"
             << "VIEWPOINT 0 0 0 1 0 0 0\n"
             << "POINTS 3\n"
             << "DATA " << dataMode << "\n";
    }

    std::vector<std::uint8_t> LzfLiteralCompress(std::vector<std::uint8_t> const& data)
    {
        std::vector<std::uint8_t> output;
        for (std::size_t index = 0; index < data.size(); index += 32)
        {
            std::size_t const count = std::min<std::size_t>(32, data.size() - index);
            output.push_back(static_cast<std::uint8_t>(count - 1));
            output.insert(output.end(), data.begin() + index, data.begin() + index + count);
        }
        return output;
    }

    void TestPcd(std::filesystem::path const& directory)
    {
        std::printf("== PCD round-trip ==\n");

        float const rgbFloat = [](std::uint32_t bits)
        {
            float value = 0.0f;
            std::memcpy(&value, &bits, 4);
            return value;
        }(0x00FF0000u); // 红色

        float const positions[3][3] = {
            { 0.0f, 0.0f, 0.0f },
            { 1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f } };

        // ascii
        {
            std::filesystem::path const path = directory / L"pcd_ascii_test.pcd";
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            WritePcdHeader(file, "ascii");
            for (auto const& p : positions)
            {
                file << p[0] << ' ' << p[1] << ' ' << p[2] << ' ' << rgbFloat << '\n';
            }
            file.close();

            app::PlyCloud const cloud = app::LoadPcdFile(path.wstring());
            Check(cloud.loadedVertexCount == 3, "pcd ascii: 3 points");
            Check(cloud.format == "pcd_ascii", "pcd ascii: format tag");
            Check(std::fabs(cloud.boundsMax.x - 1.0f) < 1.0e-5f &&
                  std::fabs(cloud.boundsMax.y - 1.0f) < 1.0e-5f,
                  "pcd ascii: bounds");
            Check(cloud.vertices[1].position.x > 0.99f, "pcd ascii: second point position");
            Check(cloud.hasColors, "pcd ascii: colors detected");
        }

        // binary
        {
            std::filesystem::path const path = directory / L"pcd_binary_test.pcd";
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            WritePcdHeader(file, "binary");
            for (auto const& p : positions)
            {
                file.write(reinterpret_cast<char const*>(&p[0]), 4);
                file.write(reinterpret_cast<char const*>(&p[1]), 4);
                file.write(reinterpret_cast<char const*>(&p[2]), 4);
                file.write(reinterpret_cast<char const*>(&rgbFloat), 4);
            }
            file.close();

            app::PlyCloud const cloud = app::LoadPcdFile(path.wstring());
            Check(cloud.loadedVertexCount == 3, "pcd binary: 3 points");
            Check(cloud.format == "pcd_binary", "pcd binary: format tag");
            Check(cloud.vertices[0].color == 0xFF0000FFu, "pcd binary: rgb packed color");
        }

        // binary_compressed（只用 LZF 字面量，验证解压路径）
        {
            std::filesystem::path const path = directory / L"pcd_compressed_test.pcd";
            std::vector<std::uint8_t> raw;
            for (int axis = 0; axis < 3; ++axis)
            {
                for (auto const& p : positions)
                {
                    raw.insert(raw.end(), reinterpret_cast<std::uint8_t const*>(&p[axis]),
                               reinterpret_cast<std::uint8_t const*>(&p[axis]) + 4);
                }
            }
            for (std::size_t i = 0; i < 3; ++i)
            {
                raw.insert(raw.end(), reinterpret_cast<std::uint8_t const*>(&rgbFloat),
                           reinterpret_cast<std::uint8_t const*>(&rgbFloat) + 4);
            }

            std::vector<std::uint8_t> const compressed = LzfLiteralCompress(raw);
            std::uint32_t const compressedSize = static_cast<std::uint32_t>(compressed.size());
            std::uint32_t const uncompressedSize = static_cast<std::uint32_t>(raw.size());

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            WritePcdHeader(file, "binary_compressed");
            file.write(reinterpret_cast<char const*>(&compressedSize), 4);
            file.write(reinterpret_cast<char const*>(&uncompressedSize), 4);
            file.write(reinterpret_cast<char const*>(compressed.data()),
                       static_cast<std::streamsize>(compressed.size()));
            file.close();

            app::PlyCloud const cloud = app::LoadPcdFile(path.wstring());
            Check(cloud.loadedVertexCount == 3, "pcd compressed: 3 points");
            Check(cloud.format == "pcd_binary_compressed", "pcd compressed: format tag");
            Check(cloud.vertices[2].position.y > 0.99f, "pcd compressed: third point position");
        }

        // 扩展名分派
        {
            app::PlyCloud const cloud = app::LoadPointCloudFile((directory / L"pcd_binary_test.pcd").wstring());
            Check(cloud.format == "pcd_binary", "LoadPointCloudFile dispatches .pcd");
        }

        // 保存 PCD 后再读回
        {
            std::filesystem::path const path = directory / L"pcd_save_test.pcd";

            std::vector<app::PlyVertex> vertices(3);
            vertices[0].position = { 0.0f, 0.0f, 0.0f };
            vertices[0].normal = { 0.0f, 0.0f, 1.0f };
            vertices[0].color = 0xFF0000FFu; // 红
            vertices[1].position = { 1.0f, 0.0f, 0.0f };
            vertices[1].normal = { 0.0f, 1.0f, 0.0f };
            vertices[1].color = 0xFF00FF00u; // 绿（R=0,G=255,B=0 -> 0xFF00FF00）
            vertices[2].position = { 0.0f, 1.0f, 0.5f };
            vertices[2].normal = { 1.0f, 0.0f, 0.0f };
            vertices[2].color = 0xFFFF0000u; // 蓝（R=0,G=0,B=255 -> 0xFFFF0000）

            app::SavePcdFile(path.wstring(), vertices);
            app::PlyCloud const cloud = app::LoadPcdFile(path.wstring());

            Check(cloud.loadedVertexCount == 3, "pcd save: round-trip count");
            Check(cloud.hasColors && cloud.hasNormals, "pcd save: colors and normals kept");
            Check(cloud.vertices[0].color == 0xFF0000FFu, "pcd save: red color round-trip");
            Check(std::fabs(cloud.vertices[1].position.x - 1.0f) < 1.0e-5f, "pcd save: position round-trip");
            Check(std::fabs(cloud.vertices[2].position.z - 0.5f) < 1.0e-5f, "pcd save: z round-trip");
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    // 临时文件（生成后再读回的 PCD/PLY）默认写到系统临时目录，避免污染仓库；
    // 需要指定目录时用 --out-dir <path>。
    std::filesystem::path outDir =
        std::filesystem::temp_directory_path() / L"3dPointCloudEditor-tests";
    std::vector<std::wstring> files;

    for (int index = 1; index < argc; ++index)
    {
        std::wstring const argument = argv[index];
        if (argument == L"--out-dir" && index + 1 < argc)
        {
            outDir = argv[++index];
            continue;
        }
        files.push_back(argument);
    }

    if (files.empty())
    {
        std::printf("usage: PlyTest [--out-dir <dir>] <file.ply> [file2.ply ...]\n");
        return 2;
    }

    std::error_code createError;
    std::filesystem::create_directories(outDir, createError);
    if (createError)
    {
        // 受限环境（沙箱/低权限令牌）可能无法在系统临时目录建目录，退回当前目录下的隐藏目录
        createError.clear();
        outDir = std::filesystem::current_path() / L".test-output";
        std::filesystem::create_directories(outDir, createError);
        if (!createError)
        {
            std::printf("note: system temp is not writable, using %ls\n", outDir.c_str());
        }
    }
    if (createError)
    {
        std::printf("cannot create output directory: %ls (%s)\n", outDir.c_str(), createError.message().c_str());
        return 2;
    }
    std::printf("scratch directory: %ls\n", outDir.c_str());

    for (std::wstring const& file : files)
    {
        std::wstring const& path = file;
        std::printf("== %ls ==\n", path.c_str());
        try
        {
            app::PlyCloud const cloud = app::LoadPlyFile(path);
            Dump("  cloud", cloud);

            Check(cloud.loadedVertexCount > 0, "loaded at least one vertex");
            Check(cloud.loadedVertexCount == cloud.declaredVertexCount, "loaded count matches header");
            Check(cloud.vertices.size() == cloud.loadedVertexCount, "vertex array matches count");

            // 包围盒合理性
            Check(cloud.boundsMin.x <= cloud.boundsMax.x &&
                  cloud.boundsMin.y <= cloud.boundsMax.y &&
                  cloud.boundsMin.z <= cloud.boundsMax.z,
                  "bounds are ordered");

            // 相机取景：包围盒中心应投影到屏幕中心附近
            app::OrbitCamera camera;
            camera.Frame(cloud.boundsMin, cloud.boundsMax);
            DirectX::XMFLOAT3 const center{
                (cloud.boundsMin.x + cloud.boundsMax.x) * 0.5f,
                (cloud.boundsMin.y + cloud.boundsMax.y) * 0.5f,
                (cloud.boundsMin.z + cloud.boundsMax.z) * 0.5f };

            DirectX::XMFLOAT4X4 viewProjection{};
            DirectX::XMStoreFloat4x4(
                &viewProjection,
                DirectX::XMMatrixMultiply(camera.View(), camera.Projection(16.0f / 9.0f)));

            DirectX::XMFLOAT3 ndc{};
            float w = 0.0f;
            bool const projected = ProjectToNdc(viewProjection, center, ndc, w);
            Check(projected, "camera sees the center");
            Check(projected && std::fabs(ndc.x) < 0.05f && std::fabs(ndc.y) < 0.05f,
                  "scene center is near screen center");

            // 包围盒角点应基本落在视锥内
            int inside = 0;
            for (int corner = 0; corner < 8; ++corner)
            {
                DirectX::XMFLOAT3 const p{
                    (corner & 1) ? cloud.boundsMax.x : cloud.boundsMin.x,
                    (corner & 2) ? cloud.boundsMax.y : cloud.boundsMin.y,
                    (corner & 4) ? cloud.boundsMax.z : cloud.boundsMin.z };
                DirectX::XMFLOAT3 cornerNdc{};
                float cornerW = 0.0f;
                if (ProjectToNdc(viewProjection, p, cornerNdc, cornerW) &&
                    std::fabs(cornerNdc.x) <= 1.0f && std::fabs(cornerNdc.y) <= 1.0f)
                {
                    ++inside;
                }
            }
            std::printf("  bbox corners inside view: %d/8\n", inside);
            Check(inside >= 6, "framed bbox mostly inside view");

            // 转动视角 / 前进 / 平移不产生 NaN
            camera.Look(120.0f, -60.0f);
            camera.Move(240.0f);
            camera.Pan(35.0f, -20.0f, 900.0f);
            DirectX::XMFLOAT3 const eye = camera.Eye();
            Check(std::isfinite(eye.x) && std::isfinite(eye.y) && std::isfinite(eye.z), "camera eye is finite after interaction");

            // 保存后再读回：顶点数、颜色、位置应保持一致
            {
                // 写到 --out-dir（默认 %TEMP%），不污染仓库
                std::filesystem::path const tempPath =
                    outDir /
                    (std::filesystem::path(path).stem().wstring() + L"_roundtrip.ply");

                app::SavePlyFile(tempPath.wstring(), cloud.vertices);
                app::PlyCloud const reloaded = app::LoadPlyFile(tempPath.wstring());

                Check(reloaded.loadedVertexCount == cloud.loadedVertexCount,
                      "save/load keeps the vertex count");
                if (cloud.hasColors)
                {
                    Check(reloaded.hasColors, "save/load keeps colors");
                }
                Check(!reloaded.vertices.empty() &&
                      std::fabs(reloaded.vertices.front().position.x - cloud.vertices.front().position.x) < 1.0e-4f &&
                      std::fabs(reloaded.vertices.front().position.y - cloud.vertices.front().position.y) < 1.0e-4f &&
                      std::fabs(reloaded.vertices.front().position.z - cloud.vertices.front().position.z) < 1.0e-4f,
                      "save/load keeps the first vertex position");

                std::filesystem::remove(tempPath);
            }
        }
        catch (std::exception const& error)
        {
            std::printf("  [FAIL] exception: %s\n", error.what());
            ++g_failures;
        }
    }

    std::printf("\nPCD tests\n");
    TestPcd(outDir);

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
