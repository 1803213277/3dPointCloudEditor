// 生成用于压力测试的二进制 PLY 点云（球面，带颜色）。
// 用法：MakeTestCloud.exe <输出文件> <点数>
// 编译（在仓库根目录执行）：cl /O2 /EHsc /std:c++20 tests\MakeTestCloud.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    void WriteFloat(std::vector<std::uint8_t>& out, float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        out.push_back(static_cast<std::uint8_t>(bits & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: MakeTestCloud <output.ply> <point-count>\n");
        return 2;
    }

    std::string const path = argv[1];
    std::uint64_t const count = std::strtoull(argv[2], nullptr, 10);
    if (count == 0)
    {
        std::printf("point-count must be > 0\n");
        return 2;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
    {
        std::printf("cannot open %s\n", path.c_str());
        return 1;
    }

    std::string header =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "comment generated large test cloud\n"
        "element vertex " + std::to_string(count) + "\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property uchar red\n"
        "property uchar green\n"
        "property uchar blue\n"
        "end_header\n";
    std::fwrite(header.data(), 1, header.size(), file);

    constexpr std::uint64_t kBatch = 500000;
    std::vector<std::uint8_t> buffer;
    buffer.reserve(static_cast<std::size_t>(kBatch) * 15);

    double const golden = 2.39996322972865332;
    for (std::uint64_t start = 0; start < count; start += kBatch)
    {
        std::uint64_t const n = (count - start < kBatch) ? (count - start) : kBatch;
        buffer.clear();
        for (std::uint64_t i = 0; i < n; ++i)
        {
            std::uint64_t const index = start + i;
            double const y = 1.0 - (2.0 * static_cast<double>(index) + 1.0) / static_cast<double>(count);
            double const radius = std::sqrt(std::fmax(0.0, 1.0 - y * y));
            double const theta = golden * static_cast<double>(index);

            WriteFloat(buffer, static_cast<float>(std::cos(theta) * radius));
            WriteFloat(buffer, static_cast<float>(y));
            WriteFloat(buffer, static_cast<float>(std::sin(theta) * radius));
            buffer.push_back(static_cast<std::uint8_t>((y + 1.0) * 127.5));
            buffer.push_back(static_cast<std::uint8_t>(std::fabs(std::cos(theta) * radius) * 255.0));
            buffer.push_back(static_cast<std::uint8_t>(std::fabs(std::sin(theta) * radius) * 255.0));
        }
        std::fwrite(buffer.data(), 1, buffer.size(), file);
    }

    std::fclose(file);
    std::printf("wrote %s: %llu points\n", path.c_str(), static_cast<unsigned long long>(count));
    return 0;
}
