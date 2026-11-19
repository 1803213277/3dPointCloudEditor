// 本文件不使用预编译头（见 3dPointCloudEditor.vcxproj 中的 PrecompiledHeader=NotUsing），
// 以便直接编译进独立测试程序 tools/PlyTest.cpp。
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <DirectXMath.h>

#include "PlyLoader.h"

namespace app
{
namespace
{
    constexpr std::size_t kBinaryChunkRecords = 65536;
    constexpr double kPi = 3.14159265358979323846;

    // -----------------------------------------------------------------------
    // 基础类型
    // -----------------------------------------------------------------------
    enum class ScalarType
    {
        Int8,
        UInt8,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Float32,
        Float64,
    };

    struct PlyProperty
    {
        std::string name;
        ScalarType type{ ScalarType::Float32 };
        bool isList{ false };
        ScalarType countType{ ScalarType::UInt8 };
    };

    struct PlyElement
    {
        std::string name;
        std::uint64_t count{ 0 };
        std::vector<PlyProperty> properties;
    };

    struct PlyHeader
    {
        std::string format;
        std::vector<PlyElement> elements;
    };

    bool TryParseScalarType(std::string const& text, ScalarType& type)
    {
        if (text == "char" || text == "int8") { type = ScalarType::Int8; return true; }
        if (text == "uchar" || text == "uint8") { type = ScalarType::UInt8; return true; }
        if (text == "short" || text == "int16") { type = ScalarType::Int16; return true; }
        if (text == "ushort" || text == "uint16") { type = ScalarType::UInt16; return true; }
        if (text == "int" || text == "int32") { type = ScalarType::Int32; return true; }
        if (text == "uint" || text == "uint32") { type = ScalarType::UInt32; return true; }
        if (text == "float" || text == "float32") { type = ScalarType::Float32; return true; }
        if (text == "double" || text == "float64") { type = ScalarType::Float64; return true; }
        return false;
    }

    std::size_t ScalarSize(ScalarType type)
    {
        switch (type)
        {
        case ScalarType::Int8:
        case ScalarType::UInt8: return 1;
        case ScalarType::Int16:
        case ScalarType::UInt16: return 2;
        case ScalarType::Int32:
        case ScalarType::UInt32:
        case ScalarType::Float32: return 4;
        case ScalarType::Float64: return 8;
        }
        return 0;
    }

    bool IsFloatType(ScalarType type)
    {
        return type == ScalarType::Float32 || type == ScalarType::Float64;
    }

    std::uint16_t Swap16(std::uint16_t value)
    {
        return static_cast<std::uint16_t>((value >> 8) | (value << 8));
    }

    std::uint32_t Swap32(std::uint32_t value)
    {
        return (value >> 24) | ((value >> 8) & 0x0000FF00u) | ((value << 8) & 0x00FF0000u) | (value << 24);
    }

    std::uint64_t Swap64(std::uint64_t value)
    {
        return (static_cast<std::uint64_t>(Swap32(static_cast<std::uint32_t>(value))) << 32) |
               static_cast<std::uint64_t>(Swap32(static_cast<std::uint32_t>(value >> 32)));
    }

    bool HostIsLittleEndian()
    {
        std::uint16_t value = 1;
        std::uint8_t first = 0;
        std::memcpy(&first, &value, 1);
        return first == 1;
    }

    double ReadScalar(std::uint8_t const* data, ScalarType type, bool swapBytes)
    {
        switch (type)
        {
        case ScalarType::Int8:
        {
            std::int8_t value = 0;
            std::memcpy(&value, data, 1);
            return value;
        }
        case ScalarType::UInt8:
        {
            std::uint8_t value = 0;
            std::memcpy(&value, data, 1);
            return value;
        }
        case ScalarType::Int16:
        {
            std::int16_t value = 0;
            std::memcpy(&value, data, 2);
            if (swapBytes) value = static_cast<std::int16_t>(Swap16(static_cast<std::uint16_t>(value)));
            return value;
        }
        case ScalarType::UInt16:
        {
            std::uint16_t value = 0;
            std::memcpy(&value, data, 2);
            if (swapBytes) value = Swap16(value);
            return value;
        }
        case ScalarType::Int32:
        {
            std::int32_t value = 0;
            std::memcpy(&value, data, 4);
            if (swapBytes) value = static_cast<std::int32_t>(Swap32(static_cast<std::uint32_t>(value)));
            return value;
        }
        case ScalarType::UInt32:
        {
            std::uint32_t value = 0;
            std::memcpy(&value, data, 4);
            if (swapBytes) value = Swap32(value);
            return value;
        }
        case ScalarType::Float32:
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, data, 4);
            if (swapBytes) bits = Swap32(bits);
            float value = 0.0f;
            std::memcpy(&value, &bits, 4);
            return value;
        }
        case ScalarType::Float64:
        {
            std::uint64_t bits = 0;
            std::memcpy(&bits, data, 8);
            if (swapBytes) bits = Swap64(bits);
            double value = 0.0;
            std::memcpy(&value, &bits, 8);
            return value;
        }
        }
        return 0.0;
    }

    // -----------------------------------------------------------------------
    // 颜色
    // -----------------------------------------------------------------------
    std::uint32_t PackBytes(float r, float g, float b)
    {
        auto toByte = [](float value) -> std::uint32_t
        {
            if (!(value > 0.0f)) return 0;
            if (value >= 1.0f) return 255;
            return static_cast<std::uint32_t>(value * 255.0f + 0.5f);
        };
        return (toByte(r) << 0) | (toByte(g) << 8) | (toByte(b) << 16) | (0xFFu << 24);
    }

    // 依据属性类型把 0..255 或 0..1 的颜色分量转换为字节颜色
    std::uint32_t PackColor(double r, double g, double b, ScalarType type)
    {
        auto normalize = [type](double value) -> float
        {
            if (!std::isfinite(value)) return 0.0f;
            if (IsFloatType(type) && value <= 1.0) value *= 255.0;
            if (value < 0.0) value = 0.0;
            if (value > 255.0) value = 255.0;
            return static_cast<float>(value / 255.0);
        };
        return PackBytes(normalize(r), normalize(g), normalize(b));
    }

    // Turbo 伪彩色：把 0..1 映射为蓝 -> 青 -> 绿 -> 黄 -> 红
    std::uint32_t TurboColor(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        float const r = 0.13572138f + t * (4.61539260f + t * (-42.66032258f + t * (132.13108234f + t * (-152.94239396f + t * 59.28637943f))));
        float const g = 0.09140261f + t * (2.19418839f + t * (4.84296658f + t * (-14.18503333f + t * (4.27729857f + t * 2.82956604f))));
        float const b = 0.10667330f + t * (12.64194608f + t * (-60.58204836f + t * (110.36276771f + t * (-89.90310912f + t * 27.34824973f))));
        return PackBytes(r, g, b);
    }

    // -----------------------------------------------------------------------
    // 头部解析
    // -----------------------------------------------------------------------
    bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    std::string TrimRight(std::string value)
    {
        while (!value.empty() && IsSpace(value.back()))
        {
            value.pop_back();
        }
        return value;
    }

    std::vector<std::string> SplitTokens(std::string const& line)
    {
        std::vector<std::string> tokens;
        std::size_t index = 0;
        while (index < line.size())
        {
            while (index < line.size() && IsSpace(line[index])) ++index;
            std::size_t const start = index;
            while (index < line.size() && !IsSpace(line[index])) ++index;
            if (index > start) tokens.emplace_back(line, start, index - start);
        }
        return tokens;
    }

    bool ParseUInt64(std::string const& text, std::uint64_t& value)
    {
        if (text.empty()) return false;
        char* end = nullptr;
        errno = 0;
        unsigned long long const parsed = std::strtoull(text.c_str(), &end, 10);
        if (errno != 0 || end == text.c_str() || *end != '\0') return false;
        value = static_cast<std::uint64_t>(parsed);
        return true;
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    PlyHeader ReadHeader(std::istream& stream)
    {
        std::string line;
        if (!std::getline(stream, line)) throw std::runtime_error("文件为空或无法读取。");
        if (TrimRight(line) != "ply") throw std::runtime_error("不是有效的 PLY 文件：缺少 'ply' 文件头。");

        PlyHeader header;
        bool sawFormat = false;
        bool sawEndHeader = false;

        while (std::getline(stream, line))
        {
            line = TrimRight(line);
            if (line.empty()) continue;

            std::vector<std::string> const tokens = SplitTokens(line);
            if (tokens.empty()) continue;

            std::string const& keyword = tokens[0];
            if (keyword == "format")
            {
                if (tokens.size() < 2) throw std::runtime_error("PLY 头部 format 行不完整。");
                header.format = tokens[1];
                sawFormat = true;
            }
            else if (keyword == "element")
            {
                if (tokens.size() < 3) throw std::runtime_error("PLY 头部 element 行不完整。");
                PlyElement element;
                element.name = tokens[1];
                if (!ParseUInt64(tokens[2], element.count))
                {
                    throw std::runtime_error("PLY 头部 element 数量无法解析：" + tokens[2]);
                }
                header.elements.push_back(std::move(element));
            }
            else if (keyword == "property")
            {
                if (header.elements.empty()) throw std::runtime_error("PLY 头部损坏：property 出现在 element 之前。");

                PlyProperty property;
                if (tokens.size() >= 2 && tokens[1] == "list")
                {
                    if (tokens.size() < 5) throw std::runtime_error("PLY 头部 list 属性定义不完整。");
                    if (!TryParseScalarType(tokens[2], property.countType) || !TryParseScalarType(tokens[3], property.type))
                    {
                        throw std::runtime_error("PLY 头部出现无法识别的属性类型。");
                    }
                    property.isList = true;
                    property.name = ToLower(tokens[4]);
                }
                else
                {
                    if (tokens.size() < 3) throw std::runtime_error("PLY 头部 property 行不完整。");
                    if (!TryParseScalarType(tokens[1], property.type))
                    {
                        throw std::runtime_error("PLY 头部出现无法识别的属性类型：" + tokens[1]);
                    }
                    property.name = ToLower(tokens[2]);
                }
                header.elements.back().properties.push_back(std::move(property));
            }
            else if (keyword == "end_header")
            {
                sawEndHeader = true;
                break;
            }
            // comment / obj_info 等其它行直接忽略
        }

        if (!sawFormat) throw std::runtime_error("PLY 头部缺少 format 行。");
        if (!sawEndHeader) throw std::runtime_error("PLY 头部缺少 end_header。");
        if (header.format != "ascii" && header.format != "binary_little_endian" && header.format != "binary_big_endian")
        {
            throw std::runtime_error("不支持的 PLY 编码格式：" + header.format);
        }
        return header;
    }

    // -----------------------------------------------------------------------
    // ASCII 数据分词器
    // -----------------------------------------------------------------------
    class TokenStream
    {
    public:
        explicit TokenStream(std::istream& stream) : m_stream(stream) {}

        bool Next(std::string& token)
        {
            for (;;)
            {
                while (m_position < m_line.size() && IsSpace(m_line[m_position])) ++m_position;
                if (m_position < m_line.size()) break;
                if (!std::getline(m_stream, m_line)) return false;
                m_position = 0;
            }

            std::size_t const start = m_position;
            while (m_position < m_line.size() && !IsSpace(m_line[m_position])) ++m_position;
            token.assign(m_line, start, m_position - start);
            return true;
        }

        double NextDouble(char const* errorMessage)
        {
            std::string token;
            if (!Next(token)) throw std::runtime_error(errorMessage);
            char* end = nullptr;
            double const value = std::strtod(token.c_str(), &end);
            if (end == token.c_str() || *end != '\0')
            {
                throw std::runtime_error("PLY 数据中包含无法解析的数值。");
            }
            return value;
        }

        long long NextInt(char const* errorMessage)
        {
            std::string token;
            if (!Next(token)) throw std::runtime_error(errorMessage);
            char* end = nullptr;
            errno = 0;
            long long const value = std::strtoll(token.c_str(), &end, 10);
            if (errno != 0 || end == token.c_str() || *end != '\0')
            {
                throw std::runtime_error("PLY 数据中包含无法解析的整数。");
            }
            return value;
        }

        void Skip(std::uint64_t count)
        {
            std::string token;
            for (std::uint64_t index = 0; index < count; ++index)
            {
                if (!Next(token)) throw std::runtime_error("PLY 数据意外结束。");
            }
        }

    private:
        std::istream& m_stream;
        std::string m_line;
        std::size_t m_position{ 0 };
    };

    bool ElementHasListProperty(PlyElement const& element)
    {
        for (auto const& property : element.properties)
        {
            if (property.isList) return true;
        }
        return false;
    }

    std::size_t ElementFixedRecordSize(PlyElement const& element)
    {
        std::size_t size = 0;
        for (auto const& property : element.properties)
        {
            size += ScalarSize(property.type);
        }
        return size;
    }

    void SkipAsciiElement(TokenStream& tokens, PlyElement const& element)
    {
        for (std::uint64_t record = 0; record < element.count; ++record)
        {
            for (auto const& property : element.properties)
            {
                if (property.isList)
                {
                    long long const count = tokens.NextInt("PLY 数据意外结束。");
                    if (count > 0) tokens.Skip(static_cast<std::uint64_t>(count));
                }
                else
                {
                    tokens.Skip(1);
                }
            }
        }
    }

    void SkipBinaryElement(std::istream& stream, PlyElement const& element, bool swapBytes)
    {
        if (!ElementHasListProperty(element))
        {
            std::uint64_t const bytes = element.count * static_cast<std::uint64_t>(ElementFixedRecordSize(element));
            stream.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
            if (!stream) throw std::runtime_error("PLY 数据意外结束。");
            return;
        }

        std::uint8_t countBuffer[8]{};
        for (std::uint64_t record = 0; record < element.count; ++record)
        {
            for (auto const& property : element.properties)
            {
                if (property.isList)
                {
                    std::size_t const countSize = ScalarSize(property.countType);
                    stream.read(reinterpret_cast<char*>(countBuffer), static_cast<std::streamsize>(countSize));
                    if (!stream) throw std::runtime_error("PLY 数据意外结束。");
                    double const countValue = ReadScalar(countBuffer, property.countType, swapBytes);
                    double const safeCount = countValue > 0.0 ? countValue : 0.0;
                    std::uint64_t const bytes =
                        static_cast<std::uint64_t>(safeCount) * static_cast<std::uint64_t>(ScalarSize(property.type));
                    stream.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
                }
                else
                {
                    stream.seekg(static_cast<std::streamoff>(ScalarSize(property.type)), std::ios::cur);
                }
                if (!stream) throw std::runtime_error("PLY 数据意外结束。");
            }
        }
    }

    // -----------------------------------------------------------------------
    // 顶点构造
    // -----------------------------------------------------------------------
    struct FieldLayout
    {
        int x{ -1 };
        int y{ -1 };
        int z{ -1 };
        int nx{ -1 };
        int ny{ -1 };
        int nz{ -1 };
        int red{ -1 };
        int green{ -1 };
        int blue{ -1 };
        int intensity{ -1 };
        std::vector<PlyProperty> const* properties{ nullptr };
        std::vector<std::size_t> offsets;

        bool HasNormals() const { return nx >= 0 && ny >= 0 && nz >= 0; }
        bool HasColors() const { return red >= 0 && green >= 0 && blue >= 0; }
    };

    int FindProperty(std::vector<PlyProperty> const& properties, std::initializer_list<char const*> names)
    {
        for (std::size_t index = 0; index < properties.size(); ++index)
        {
            for (char const* name : names)
            {
                if (properties[index].name == name) return static_cast<int>(index);
            }
        }
        return -1;
    }

    template <typename GetValue>
    bool AppendVertex(PlyCloud& cloud, FieldLayout const& layout, GetValue&& value, bool& needsColorRamp)
    {
        double const px = value(layout.x);
        double const py = value(layout.y);
        double const pz = value(layout.z);
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) return false;

        PlyVertex vertex;
        vertex.position = { static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz) };

        if (layout.HasNormals())
        {
            float nx = static_cast<float>(value(layout.nx));
            float ny = static_cast<float>(value(layout.ny));
            float nz = static_cast<float>(value(layout.nz));
            float const length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (std::isfinite(length) && length > 1.0e-6f)
            {
                vertex.normal = { nx / length, ny / length, nz / length };
            }
        }

        if (layout.HasColors())
        {
            auto const type = (*layout.properties)[layout.red].type;
            vertex.color = PackColor(value(layout.red), value(layout.green), value(layout.blue), type);
        }
        else if (layout.intensity >= 0)
        {
            auto const type = (*layout.properties)[layout.intensity].type;
            double const intensity = value(layout.intensity);
            vertex.color = PackColor(intensity, intensity, intensity, type);
        }
        else
        {
            needsColorRamp = true;
        }

        cloud.boundsMin.x = std::min(cloud.boundsMin.x, vertex.position.x);
        cloud.boundsMin.y = std::min(cloud.boundsMin.y, vertex.position.y);
        cloud.boundsMin.z = std::min(cloud.boundsMin.z, vertex.position.z);
        cloud.boundsMax.x = std::max(cloud.boundsMax.x, vertex.position.x);
        cloud.boundsMax.y = std::max(cloud.boundsMax.y, vertex.position.y);
        cloud.boundsMax.z = std::max(cloud.boundsMax.z, vertex.position.z);

        cloud.vertices.push_back(vertex);
        return true;
    }
} // namespace

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------
PlyCloud LoadPlyFile(std::wstring const& path)
{
    std::ifstream stream(path.c_str(), std::ios::binary);
    if (!stream) throw std::runtime_error("无法打开文件（路径不存在或没有访问权限）。");

    stream.seekg(0, std::ios::end);
    std::streamoff const fileSize = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (fileSize <= 0) throw std::runtime_error("文件为空。");

    PlyHeader const header = ReadHeader(stream);

    std::size_t vertexElementIndex = header.elements.size();
    for (std::size_t index = 0; index < header.elements.size(); ++index)
    {
        if (header.elements[index].name == "vertex")
        {
            vertexElementIndex = index;
            break;
        }
    }
    if (vertexElementIndex == header.elements.size()) throw std::runtime_error("PLY 文件中没有 vertex 元素。");

    PlyElement const& vertexElement = header.elements[vertexElementIndex];
    if (vertexElement.count == 0) throw std::runtime_error("PLY 文件中没有顶点。");

    // 不限制点数上限，只做平台可寻址范围的保护（点云仍受内存/显存大小限制）。
    constexpr std::size_t kMaxAddressable =
        (std::numeric_limits<std::size_t>::max)() / sizeof(PlyVertex);
    if (vertexElement.count > static_cast<std::uint64_t>(kMaxAddressable))
    {
        throw std::runtime_error("顶点数量超出当前平台可寻址的内存范围。");
    }
    if (ElementHasListProperty(vertexElement))
    {
        throw std::runtime_error("暂不支持带 list 属性的 vertex 元素。");
    }

    FieldLayout layout;
    layout.properties = &vertexElement.properties;
    layout.x = FindProperty(vertexElement.properties, { "x" });
    layout.y = FindProperty(vertexElement.properties, { "y" });
    layout.z = FindProperty(vertexElement.properties, { "z" });
    if (layout.x < 0 || layout.y < 0 || layout.z < 0)
    {
        throw std::runtime_error("PLY 顶点缺少 x/y/z 坐标属性。");
    }
    layout.nx = FindProperty(vertexElement.properties, { "nx", "normal_x" });
    layout.ny = FindProperty(vertexElement.properties, { "ny", "normal_y" });
    layout.nz = FindProperty(vertexElement.properties, { "nz", "normal_z" });
    layout.red = FindProperty(vertexElement.properties, { "red", "diffuse_red" });
    layout.green = FindProperty(vertexElement.properties, { "green", "diffuse_green" });
    layout.blue = FindProperty(vertexElement.properties, { "blue", "diffuse_blue" });
    layout.intensity = FindProperty(vertexElement.properties, { "intensity", "scalar", "scalar_intensity", "gray", "grey" });

    std::size_t recordSize = 0;
    layout.offsets.resize(vertexElement.properties.size());
    for (std::size_t index = 0; index < vertexElement.properties.size(); ++index)
    {
        layout.offsets[index] = recordSize;
        recordSize += ScalarSize(vertexElement.properties[index].type);
    }

    bool const hasColors = layout.HasColors();
    bool const hasNormals = layout.HasNormals();
    bool const isBinary = header.format == "binary_little_endian" || header.format == "binary_big_endian";
    bool const swapBytes = isBinary && ((header.format == "binary_little_endian") != HostIsLittleEndian());

    PlyCloud cloud;
    cloud.format = header.format;
    cloud.declaredVertexCount = vertexElement.count;
    cloud.hasColors = hasColors;
    cloud.hasNormals = hasNormals;
    cloud.fileSizeBytes = static_cast<std::uint64_t>(fileSize);
    cloud.boundsMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    cloud.boundsMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    // 一次性按头部声明的数量预留内存（避免增长时额外的拷贝峰值）；
    // 内存不够时给出明确提示，而不是抛出 std::bad_alloc。
    try
    {
        cloud.vertices.reserve(static_cast<std::size_t>(vertexElement.count));
    }
    catch (std::bad_alloc const&)
    {
        std::uint64_t const requiredGb =
            (vertexElement.count * sizeof(PlyVertex)) / (1024ull * 1024ull * 1024ull);
        throw std::runtime_error(
            "内存不足：无法为 " + std::to_string(vertexElement.count) +
            " 个顶点预留内存（约 " + std::to_string(requiredGb) + " GB）。");
    }

    bool needsColorRamp = false;

    if (!isBinary)
    {
        TokenStream tokens(stream);
        for (std::size_t index = 0; index < vertexElementIndex; ++index)
        {
            SkipAsciiElement(tokens, header.elements[index]);
        }

        std::vector<double> values(vertexElement.properties.size(), 0.0);
        for (std::uint64_t record = 0; record < vertexElement.count; ++record)
        {
            for (std::size_t property = 0; property < values.size(); ++property)
            {
                values[property] = tokens.NextDouble("PLY 数据意外结束（顶点数据不完整）。");
            }
            AppendVertex(
                cloud,
                layout,
                [&values](int index) { return values[static_cast<std::size_t>(index)]; },
                needsColorRamp);
        }
    }
    else
    {
        for (std::size_t index = 0; index < vertexElementIndex; ++index)
        {
            SkipBinaryElement(stream, header.elements[index], swapBytes);
        }

        std::vector<std::uint8_t> buffer(recordSize * kBinaryChunkRecords, 0);
        std::uint64_t remaining = vertexElement.count;
        while (remaining > 0)
        {
            std::size_t const chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kBinaryChunkRecords));
            std::streamsize const bytes = static_cast<std::streamsize>(chunk * recordSize);
            stream.read(reinterpret_cast<char*>(buffer.data()), bytes);
            if (stream.gcount() != bytes)
            {
                throw std::runtime_error("PLY 数据被截断（二进制顶点数据不完整）。");
            }

            for (std::size_t record = 0; record < chunk; ++record)
            {
                std::uint8_t const* const data = buffer.data() + record * recordSize;
                AppendVertex(
                    cloud,
                    layout,
                    [data, &layout, swapBytes](int index)
                    {
                        auto const property = static_cast<std::size_t>(index);
                        return ReadScalar(data + layout.offsets[property], (*layout.properties)[property].type, swapBytes);
                    },
                    needsColorRamp);
            }
            remaining -= chunk;
        }
    }

    if (cloud.vertices.empty()) throw std::runtime_error("未能从文件中读取到有效顶点。");

    if (needsColorRamp)
    {
        float const range = cloud.boundsMax.z - cloud.boundsMin.z;
        float const safeRange = range > 0.0f ? range : 1.0f;
        for (auto& vertex : cloud.vertices)
        {
            vertex.color = TurboColor((vertex.position.z - cloud.boundsMin.z) / safeRange);
        }
    }

    cloud.loadedVertexCount = cloud.vertices.size();
    return cloud;
}

PlyCloud MakeDemoPointCloud()
{
    constexpr std::size_t kPointCount = 150000;
    constexpr double kGolden = 0.6180339887498949;
    constexpr double kMajorRadius = 1.0;
    constexpr double kMinorRadius = 0.38;

    PlyCloud cloud;
    cloud.format = "generated";
    cloud.hasColors = true;
    cloud.hasNormals = true;
    cloud.vertices.resize(kPointCount);

    for (std::size_t index = 0; index < kPointCount; ++index)
    {
        double const u = 2.0 * kPi * static_cast<double>(index) / static_cast<double>(kPointCount);
        double const v = 2.0 * kPi * std::fmod(static_cast<double>(index) * kGolden, 1.0);
        double const cu = std::cos(u);
        double const su = std::sin(u);
        double const cv = std::cos(v);
        double const sv = std::sin(v);
        double const ring = kMajorRadius + kMinorRadius * cv;

        PlyVertex& vertex = cloud.vertices[index];
        // 环面躺在 XY 平面上（Z 轴向上）
        vertex.position = {
            static_cast<float>(ring * cu),
            static_cast<float>(ring * su),
            static_cast<float>(kMinorRadius * sv) };
        vertex.normal = {
            static_cast<float>(cv * cu),
            static_cast<float>(cv * su),
            static_cast<float>(sv) };
        vertex.color = TurboColor(static_cast<float>(u / (2.0 * kPi)));
    }

    float const extent = static_cast<float>(kMajorRadius + kMinorRadius);
    cloud.boundsMin = { -extent, -extent, static_cast<float>(-kMinorRadius) };
    cloud.boundsMax = { extent, extent, static_cast<float>(kMinorRadius) };
    cloud.declaredVertexCount = kPointCount;
    cloud.loadedVertexCount = kPointCount;
    return cloud;
}

void SavePlyFile(std::wstring const& path, std::vector<PlyVertex> const& vertices)
{
    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!file)
    {
        throw std::runtime_error(
            "无法创建 PLY 文件（路径不可写，errno=" + std::to_string(errno) + "）。");
    }

    std::string header =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "comment saved by PLY point cloud viewer\n"
        "element vertex " + std::to_string(vertices.size()) + "\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property float nx\n"
        "property float ny\n"
        "property float nz\n"
        "property uchar red\n"
        "property uchar green\n"
        "property uchar blue\n"
        "end_header\n";
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!file)
    {
        throw std::runtime_error("写入 PLY 文件头失败。");
    }

    constexpr std::size_t kVertexBytes = 27; // 6 * float + 3 * uchar
    constexpr std::size_t kBatchVertices = 32768;
    std::vector<unsigned char> buffer;
    buffer.reserve(kBatchVertices * kVertexBytes);

    auto const appendFloat = [](std::vector<unsigned char>& target, float value)
    {
        unsigned char const* bytes = reinterpret_cast<unsigned char const*>(&value);
        target.insert(target.end(), bytes, bytes + sizeof(float));
    };

    auto flush = [&]()
    {
        if (buffer.empty()) return;
        file.write(
            reinterpret_cast<char const*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        buffer.clear();
    };

    for (PlyVertex const& vertex : vertices)
    {
        appendFloat(buffer, vertex.position.x);
        appendFloat(buffer, vertex.position.y);
        appendFloat(buffer, vertex.position.z);
        appendFloat(buffer, vertex.normal.x);
        appendFloat(buffer, vertex.normal.y);
        appendFloat(buffer, vertex.normal.z);

        buffer.push_back(static_cast<unsigned char>(vertex.color & 0xFFu));
        buffer.push_back(static_cast<unsigned char>((vertex.color >> 8) & 0xFFu));
        buffer.push_back(static_cast<unsigned char>((vertex.color >> 16) & 0xFFu));

        if (buffer.size() >= kBatchVertices * kVertexBytes)
        {
            flush();
        }
    }
    flush();

    file.flush();
    if (!file)
    {
        throw std::runtime_error("写入 PLY 顶点数据失败（磁盘空间不足？）。");
    }
}

// ---------------------------------------------------------------------------
// PCD（Point Cloud Data，PCL 格式）
// ---------------------------------------------------------------------------
namespace
{
    struct PcdHeader
    {
        std::string data;
        std::vector<std::string> fields;
        std::vector<int> sizes;
        std::vector<char> types;
        std::vector<int> counts;
        std::uint64_t width{ 0 };
        std::uint64_t height{ 1 };
        std::uint64_t points{ 0 };
    };

    std::vector<std::string> SplitPcdTokens(std::string const& line)
    {
        std::vector<std::string> tokens;
        std::size_t index = 0;
        while (index < line.size())
        {
            while (index < line.size() &&
                   (line[index] == ' ' || line[index] == '\t' || line[index] == '\r'))
            {
                ++index;
            }
            std::size_t const start = index;
            while (index < line.size() &&
                   line[index] != ' ' && line[index] != '\t' && line[index] != '\r')
            {
                ++index;
            }
            if (index > start)
            {
                tokens.push_back(line.substr(start, index - start));
            }
        }
        return tokens;
    }

    PcdHeader ReadPcdHeader(std::istream& stream)
    {
        PcdHeader header;
        std::string line;
        bool dataSeen = false;

        while (std::getline(stream, line))
        {
            auto const tokens = SplitPcdTokens(line);
            if (tokens.empty() || tokens[0][0] == '#') continue;

            std::string const& key = tokens[0];
            if (key == "FIELDS" || key == "COLUMNS")
            {
                header.fields.assign(tokens.begin() + 1, tokens.end());
            }
            else if (key == "SIZE")
            {
                header.sizes.clear();
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    header.sizes.push_back(std::atoi(tokens[i].c_str()));
                }
            }
            else if (key == "TYPE")
            {
                header.types.clear();
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    header.types.push_back(tokens[i].empty() ? 'F' : tokens[i][0]);
                }
            }
            else if (key == "COUNT")
            {
                header.counts.clear();
                for (std::size_t i = 1; i < tokens.size(); ++i)
                {
                    header.counts.push_back(std::atoi(tokens[i].c_str()));
                }
            }
            else if (key == "WIDTH")
            {
                header.width = tokens.size() > 1 ? std::strtoull(tokens[1].c_str(), nullptr, 10) : 0;
            }
            else if (key == "HEIGHT")
            {
                header.height = tokens.size() > 1 ? std::strtoull(tokens[1].c_str(), nullptr, 10) : 1;
            }
            else if (key == "POINTS")
            {
                header.points = tokens.size() > 1 ? std::strtoull(tokens[1].c_str(), nullptr, 10) : 0;
            }
            else if (key == "DATA")
            {
                header.data = tokens.size() > 1 ? tokens[1] : "ascii";
                dataSeen = true;
                break;
            }
        }

        if (!dataSeen)
        {
            throw std::runtime_error("PCD 文件缺少 DATA 行。");
        }
        if (header.points == 0)
        {
            header.points = header.width * (header.height > 0 ? header.height : 1);
        }
        return header;
    }

    double ReadPcdScalar(std::uint8_t const* data, char type, int size)
    {
        switch (type)
        {
        case 'F':
        case 'f':
            if (size == 4)
            {
                float value = 0.0f;
                std::memcpy(&value, data, 4);
                return value;
            }
            if (size == 8)
            {
                double value = 0.0;
                std::memcpy(&value, data, 8);
                return value;
            }
            break;
        case 'U':
        case 'u':
            if (size == 1) return data[0];
            if (size == 2)
            {
                std::uint16_t value = 0;
                std::memcpy(&value, data, 2);
                return value;
            }
            if (size == 4)
            {
                std::uint32_t value = 0;
                std::memcpy(&value, data, 4);
                return value;
            }
            if (size == 8)
            {
                std::uint64_t value = 0;
                std::memcpy(&value, data, 8);
                return static_cast<double>(value);
            }
            break;
        case 'I':
        case 'i':
            if (size == 1) return static_cast<std::int8_t>(data[0]);
            if (size == 2)
            {
                std::int16_t value = 0;
                std::memcpy(&value, data, 2);
                return value;
            }
            if (size == 4)
            {
                std::int32_t value = 0;
                std::memcpy(&value, data, 4);
                return value;
            }
            if (size == 8)
            {
                std::int64_t value = 0;
                std::memcpy(&value, data, 8);
                return static_cast<double>(value);
            }
            break;
        default:
            break;
        }
        return 0.0;
    }

    // LZF 解压（PCD binary_compressed 使用）
    std::vector<std::uint8_t> LzfDecompress(
        std::uint8_t const* input,
        std::size_t inputSize,
        std::size_t outputSize)
    {
        std::vector<std::uint8_t> output(outputSize);
        std::size_t inIndex = 0;
        std::size_t outIndex = 0;

        while (inIndex < inputSize)
        {
            std::uint8_t const control = input[inIndex++];
            if (control < 32)
            {
                std::size_t const length = static_cast<std::size_t>(control) + 1;
                if (inIndex + length > inputSize || outIndex + length > outputSize)
                {
                    throw std::runtime_error("PCD 压缩数据损坏（LZF 字面量越界）。");
                }
                std::memcpy(output.data() + outIndex, input + inIndex, length);
                inIndex += length;
                outIndex += length;
            }
            else
            {
                std::size_t length = control >> 5;
                if (length == 7)
                {
                    if (inIndex >= inputSize) throw std::runtime_error("PCD 压缩数据损坏（LZF）。");
                    length += input[inIndex++];
                }
                length += 2;

                if (inIndex >= inputSize) throw std::runtime_error("PCD 压缩数据损坏（LZF）。");
                std::size_t const reference =
                    outIndex - ((static_cast<std::size_t>(control) & 0x1Fu) << 8) - 1;

                std::size_t back = static_cast<std::size_t>(input[inIndex++]);
                if (back > reference) throw std::runtime_error("PCD 压缩数据损坏（LZF 引用越界）。");
                std::size_t const start = reference - back;
                if (start >= outIndex || outIndex + length > outputSize)
                {
                    throw std::runtime_error("PCD 压缩数据损坏（LZF 引用越界）。");
                }

                for (std::size_t i = 0; i < length; ++i)
                {
                    output[outIndex] = output[start + i];
                    ++outIndex;
                }
            }
        }
        return output;
    }

    struct PcdLayout
    {
        int x{ -1 };
        int y{ -1 };
        int z{ -1 };
        int nx{ -1 };
        int ny{ -1 };
        int nz{ -1 };
        int rgb{ -1 };
        int rgba{ -1 };
        int intensity{ -1 };
        char intensityType{ 'F' };
        int intensitySize{ 4 };
        float intensityScale{ 1.0f };

        std::vector<int> counts;
        std::vector<char> types;
        std::vector<int> sizes;
        std::vector<std::size_t> recordOffsets;   // 二进制：每条记录的字段偏移
        std::vector<std::size_t> componentOffsets; // ASCII：每个字段在一条记录里的第几个分量
        std::vector<std::size_t> fieldOffsets;     // compressed：每个字段数组的起始偏移
        std::size_t recordSize{ 0 };
        std::size_t totalComponents{ 0 };
        int colorField() const { return rgb >= 0 ? rgb : rgba; }
        bool hasColor() const { return colorField() >= 0; }
    };

    void AppendPcdVertex(
        PlyCloud& cloud,
        PcdLayout const& layout,
        double px,
        double py,
        double pz,
        double nx,
        double ny,
        double nz,
        std::uint32_t rgbBits,
        double intensity,
        bool& needsColorRamp)
    {
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) return;

        PlyVertex vertex;
        vertex.position = {
            static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz) };

        if (layout.nx >= 0)
        {
            float nxv = static_cast<float>(nx);
            float nyv = static_cast<float>(ny);
            float nzv = static_cast<float>(nz);
            float const length = std::sqrt(nxv * nxv + nyv * nyv + nzv * nzv);
            if (std::isfinite(length) && length > 1.0e-6f)
            {
                vertex.normal = { nxv / length, nyv / length, nzv / length };
            }
        }

        if (layout.hasColor())
        {
            std::uint32_t const r = (rgbBits >> 16) & 0xFFu;
            std::uint32_t const g = (rgbBits >> 8) & 0xFFu;
            std::uint32_t const b = rgbBits & 0xFFu;
            std::uint32_t const a = (rgbBits >> 24) & 0xFFu;
            vertex.color = r | (g << 8) | (b << 16) | ((a == 0u ? 0xFFu : a) << 24);
        }
        else if (layout.intensity >= 0)
        {
            double normalized = intensity;
            if (layout.intensityType == 'F' || layout.intensityType == 'f')
            {
                normalized = std::max(0.0, std::min(1.0, intensity)) * 255.0;
            }
            else if (layout.intensitySize == 2)
            {
                normalized = intensity * 255.0 / 65535.0;
            }
            else if (layout.intensitySize == 4)
            {
                normalized = intensity * 255.0 / 4294967295.0;
            }
            int const gray = static_cast<int>(
                std::lround(std::max(0.0, std::min(255.0, normalized))));
            std::uint32_t const value = static_cast<std::uint32_t>(gray);
            vertex.color = value | (value << 8) | (value << 16) | 0xFF000000u;
        }
        else
        {
            needsColorRamp = true;
        }

        cloud.boundsMin.x = std::min(cloud.boundsMin.x, vertex.position.x);
        cloud.boundsMin.y = std::min(cloud.boundsMin.y, vertex.position.y);
        cloud.boundsMin.z = std::min(cloud.boundsMin.z, vertex.position.z);
        cloud.boundsMax.x = std::max(cloud.boundsMax.x, vertex.position.x);
        cloud.boundsMax.y = std::max(cloud.boundsMax.y, vertex.position.y);
        cloud.boundsMax.z = std::max(cloud.boundsMax.z, vertex.position.z);

        cloud.vertices.push_back(vertex);
    }

    std::uint32_t RgbBitsFromFloat(double value)
    {
        float const asFloat = static_cast<float>(value);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &asFloat, 4);
        return bits;
    }

    std::uint32_t RgbBitsFromAscii(double value, char type)
    {
        // 浮点字段按 PCD 约定（float 的位模式就是颜色）；整数/短整型字段直接当数值
        if (type == 'F' || type == 'f')
        {
            return RgbBitsFromFloat(value);
        }
        return static_cast<std::uint32_t>(value);
    }
}

PlyCloud LoadPcdFile(std::wstring const& path)
{
    std::ifstream stream(path.c_str(), std::ios::binary);
    if (!stream) throw std::runtime_error("无法打开 PCD 文件（路径不存在或没有访问权限）。");

    stream.seekg(0, std::ios::end);
    std::streamoff const fileSize = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (fileSize <= 0) throw std::runtime_error("文件为空。");

    PcdHeader const header = ReadPcdHeader(stream);
    std::size_t const fieldCount = header.fields.size();
    if (fieldCount == 0 ||
        header.sizes.size() != fieldCount ||
        header.types.size() != fieldCount ||
        header.counts.size() != fieldCount)
    {
        throw std::runtime_error("PCD 头部的 FIELDS / SIZE / TYPE / COUNT 数量不一致。");
    }

    PcdLayout layout;
    layout.counts = header.counts;
    layout.types = header.types;
    layout.sizes = header.sizes;

    for (std::size_t i = 0; i < fieldCount; ++i)
    {
        std::string const& name = header.fields[i];
        int const index = static_cast<int>(i);
        if (name == "x") layout.x = index;
        else if (name == "y") layout.y = index;
        else if (name == "z") layout.z = index;
        else if (name == "normal_x") layout.nx = index;
        else if (name == "normal_y") layout.ny = index;
        else if (name == "normal_z") layout.nz = index;
        else if (name == "rgb") layout.rgb = index;
        else if (name == "rgba") layout.rgba = index;
        else if (name == "intensity")
        {
            layout.intensity = index;
            layout.intensityType = header.types[i];
            layout.intensitySize = header.sizes[i];
        }
    }

    if (layout.x < 0 || layout.y < 0 || layout.z < 0)
    {
        throw std::runtime_error("PCD 文件缺少 x/y/z 字段。");
    }
    if (header.points == 0)
    {
        throw std::runtime_error("PCD 文件中没有点（POINTS 为 0）。");
    }

    // 字段偏移（二进制记录内 / ASCII 分量 / 压缩后的字段数组）
    std::size_t recordSize = 0;
    std::size_t totalComponents = 0;
    std::size_t fieldOffset = 0;
    layout.recordOffsets.resize(fieldCount);
    layout.componentOffsets.resize(fieldCount);
    layout.fieldOffsets.resize(fieldCount);
    for (std::size_t i = 0; i < fieldCount; ++i)
    {
        int const count = std::max(1, header.counts[i]);
        std::size_t const size = static_cast<std::size_t>(std::max(1, header.sizes[i]));
        layout.recordOffsets[i] = recordSize;
        layout.componentOffsets[i] = totalComponents;
        layout.fieldOffsets[i] = fieldOffset;
        recordSize += size * static_cast<std::size_t>(count);
        totalComponents += static_cast<std::size_t>(count);
        fieldOffset += static_cast<std::size_t>(header.points) * size * static_cast<std::size_t>(count);
    }
    layout.recordSize = recordSize;
    layout.totalComponents = totalComponents;

    if (recordSize == 0) throw std::runtime_error("PCD 字段大小无效。");

    constexpr std::size_t kMaxAddressable =
        (std::numeric_limits<std::size_t>::max)() / sizeof(PlyVertex);
    if (header.points > static_cast<std::uint64_t>(kMaxAddressable))
    {
        throw std::runtime_error("顶点数量超出当前平台可寻址的内存范围。");
    }

    PlyCloud cloud;
    cloud.format = "pcd_" + header.data;
    cloud.declaredVertexCount = header.points;
    cloud.hasNormals = layout.nx >= 0;
    cloud.hasColors = layout.hasColor() || layout.intensity >= 0;
    cloud.fileSizeBytes = static_cast<std::uint64_t>(fileSize);
    cloud.boundsMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    cloud.boundsMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    try
    {
        cloud.vertices.reserve(static_cast<std::size_t>(header.points));
    }
    catch (std::bad_alloc const&)
    {
        std::uint64_t const requiredGb =
            (static_cast<std::uint64_t>(header.points) * sizeof(PlyVertex)) / (1024ull * 1024ull * 1024ull);
        throw std::runtime_error(
            "内存不足：无法为 " + std::to_string(header.points) +
            " 个顶点预留内存（约 " + std::to_string(requiredGb) + " GB）。");
    }

    bool needsColorRamp = false;
    int const colorIndex = layout.colorField();

    if (header.data == "ascii")
    {
        TokenStream tokens(stream);
        std::vector<double> values(totalComponents, 0.0);
        for (std::uint64_t point = 0; point < header.points; ++point)
        {
            for (std::size_t i = 0; i < totalComponents; ++i)
            {
                values[i] = tokens.NextDouble("PCD 数据意外结束（点数据不完整）。");
            }

            double const px = values[static_cast<std::size_t>(layout.componentOffsets[static_cast<std::size_t>(layout.x)])];
            double const py = values[static_cast<std::size_t>(layout.componentOffsets[static_cast<std::size_t>(layout.y)])];
            double const pz = values[static_cast<std::size_t>(layout.componentOffsets[static_cast<std::size_t>(layout.z)])];

            std::uint32_t rgbBits = 0;
            if (colorIndex >= 0)
            {
                std::size_t const component =
                    static_cast<std::size_t>(layout.componentOffsets[static_cast<std::size_t>(colorIndex)]);
                rgbBits = RgbBitsFromAscii(values[component], header.types[static_cast<std::size_t>(colorIndex)]);
            }

            double intensity = 0.0;
            if (layout.intensity >= 0)
            {
                intensity = values[static_cast<std::size_t>(
                    layout.componentOffsets[static_cast<std::size_t>(layout.intensity)])];
            }

            double nx = 0.0, ny = 0.0, nz = 0.0;
            if (layout.nx >= 0)
            {
                nx = values[static_cast<std::size_t>(layout.componentOffsets[static_cast<std::size_t>(layout.nx)])];
                ny = values[static_cast<std::size_t>(layout.componentOffsets[static_cast<std::size_t>(layout.ny)])];
                nz = values[static_cast<std::size_t>(layout.componentOffsets[static_cast<std::size_t>(layout.nz)])];
            }

            AppendPcdVertex(cloud, layout, px, py, pz, nx, ny, nz, rgbBits, intensity, needsColorRamp);
        }
    }
    else if (header.data == "binary")
    {
        std::vector<std::uint8_t> buffer(recordSize * kBinaryChunkRecords, 0);
        std::uint64_t remaining = header.points;
        while (remaining > 0)
        {
            std::size_t const chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, kBinaryChunkRecords));
            std::streamsize const bytes = static_cast<std::streamsize>(chunk * recordSize);
            stream.read(reinterpret_cast<char*>(buffer.data()), bytes);
            if (stream.gcount() != bytes)
            {
                throw std::runtime_error("PCD 数据被截断（二进制点数据不完整）。");
            }

            for (std::size_t record = 0; record < chunk; ++record)
            {
                std::uint8_t const* const data = buffer.data() + record * recordSize;

                auto scalar = [&](int field) -> double
                {
                    auto const index = static_cast<std::size_t>(field);
                    return ReadPcdScalar(
                        data + layout.recordOffsets[index],
                        header.types[index],
                        header.sizes[index]);
                };

                double const px = scalar(layout.x);
                double const py = scalar(layout.y);
                double const pz = scalar(layout.z);

                std::uint32_t rgbBits = 0;
                if (colorIndex >= 0)
                {
                    auto const index = static_cast<std::size_t>(colorIndex);
                    std::memcpy(&rgbBits, data + layout.recordOffsets[index], 4);
                }

                double const intensity = layout.intensity >= 0 ? scalar(layout.intensity) : 0.0;
                double const nx = layout.nx >= 0 ? scalar(layout.nx) : 0.0;
                double const ny = layout.ny >= 0 ? scalar(layout.ny) : 0.0;
                double const nz = layout.nz >= 0 ? scalar(layout.nz) : 0.0;

                AppendPcdVertex(cloud, layout, px, py, pz, nx, ny, nz, rgbBits, intensity, needsColorRamp);
            }
            remaining -= chunk;
        }
    }
    else if (header.data == "binary_compressed")
    {
        std::uint32_t compressedSize = 0;
        std::uint32_t uncompressedSize = 0;
        stream.read(reinterpret_cast<char*>(&compressedSize), 4);
        stream.read(reinterpret_cast<char*>(&uncompressedSize), 4);
        if (!stream || compressedSize == 0 || uncompressedSize == 0)
        {
            throw std::runtime_error("PCD 压缩数据头无效。");
        }

        std::vector<std::uint8_t> compressed(compressedSize);
        stream.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressedSize));
        if (stream.gcount() != static_cast<std::streamsize>(compressedSize))
        {
            throw std::runtime_error("PCD 压缩数据被截断。");
        }

        std::vector<std::uint8_t> const data = LzfDecompress(
            compressed.data(), compressed.size(), uncompressedSize);
        if (data.size() != fieldOffset)
        {
            throw std::runtime_error("PCD 压缩数据大小与字段布局不一致。");
        }

        auto fieldScalar = [&](int field, std::uint64_t point) -> double
        {
            auto const index = static_cast<std::size_t>(field);
            std::size_t const offset = layout.fieldOffsets[index] +
                static_cast<std::size_t>(point) * static_cast<std::size_t>(header.sizes[index]);
            return ReadPcdScalar(
                data.data() + offset, header.types[index], header.sizes[index]);
        };

        for (std::uint64_t point = 0; point < header.points; ++point)
        {
            double const px = fieldScalar(layout.x, point);
            double const py = fieldScalar(layout.y, point);
            double const pz = fieldScalar(layout.z, point);

            std::uint32_t rgbBits = 0;
            if (colorIndex >= 0)
            {
                auto const index = static_cast<std::size_t>(colorIndex);
                std::size_t const offset = layout.fieldOffsets[index] +
                    static_cast<std::size_t>(point) * static_cast<std::size_t>(header.sizes[index]);
                std::memcpy(&rgbBits, data.data() + offset, 4);
            }

            double const intensity = layout.intensity >= 0 ? fieldScalar(layout.intensity, point) : 0.0;
            double const nx = layout.nx >= 0 ? fieldScalar(layout.nx, point) : 0.0;
            double const ny = layout.ny >= 0 ? fieldScalar(layout.ny, point) : 0.0;
            double const nz = layout.nz >= 0 ? fieldScalar(layout.nz, point) : 0.0;

            AppendPcdVertex(cloud, layout, px, py, pz, nx, ny, nz, rgbBits, intensity, needsColorRamp);
        }
    }
    else
    {
        throw std::runtime_error(
            "不支持的 PCD DATA 类型（仅支持 ascii / binary / binary_compressed）。");
    }

    if (cloud.vertices.empty()) throw std::runtime_error("未能从 PCD 文件中读取到有效顶点。");

    if (needsColorRamp)
    {
        float const range = cloud.boundsMax.z - cloud.boundsMin.z;
        float const safeRange = range > 0.0f ? range : 1.0f;
        for (auto& vertex : cloud.vertices)
        {
            vertex.color = TurboColor((vertex.position.z - cloud.boundsMin.z) / safeRange);
        }
    }

    cloud.loadedVertexCount = cloud.vertices.size();
    return cloud;
}

PlyCloud LoadPointCloudFile(std::wstring const& path)
{
    std::wstring lower = path;
    for (auto& ch : lower)
    {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }

    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".pcd") == 0)
    {
        return LoadPcdFile(path);
    }
    return LoadPlyFile(path);
}

void SavePcdFile(std::wstring const& path, std::vector<PlyVertex> const& vertices)
{
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file)
    {
        throw std::runtime_error(
            "无法创建 PCD 文件（路径不可写，errno=" + std::to_string(errno) + "）。");
    }

    std::string header =
        "# .PCD v0.7 - Point Cloud Data file format\n"
        "VERSION 0.7\n"
        "FIELDS x y z rgb normal_x normal_y normal_z\n"
        "SIZE 4 4 4 4 4 4 4\n"
        "TYPE F F F F F F F\n"
        "COUNT 1 1 1 1 1 1 1\n"
        "WIDTH " + std::to_string(vertices.size()) + "\n"
        "HEIGHT 1\n"
        "VIEWPOINT 0 0 0 1 0 0 0\n"
        "POINTS " + std::to_string(vertices.size()) + "\n"
        "DATA binary\n";
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!file)
    {
        throw std::runtime_error("写入 PCD 文件头失败。");
    }

    constexpr std::size_t kPointBytes = 28; // 3 float + rgb(float) + 3 float
    constexpr std::size_t kBatchPoints = 32768;
    std::vector<unsigned char> buffer;
    buffer.reserve(kBatchPoints * kPointBytes);

    auto appendFloat = [](std::vector<unsigned char>& target, float value)
    {
        unsigned char const* bytes = reinterpret_cast<unsigned char const*>(&value);
        target.insert(target.end(), bytes, bytes + sizeof(float));
    };

    auto flush = [&]()
    {
        if (buffer.empty()) return;
        file.write(
            reinterpret_cast<char const*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        buffer.clear();
    };

    for (PlyVertex const& vertex : vertices)
    {
        appendFloat(buffer, vertex.position.x);
        appendFloat(buffer, vertex.position.y);
        appendFloat(buffer, vertex.position.z);

        // 颜色：PCD 的 rgb 字段是 0x00RRGGBB 的位模式（以 float 存储）
        std::uint32_t const r = vertex.color & 0xFFu;
        std::uint32_t const g = (vertex.color >> 8) & 0xFFu;
        std::uint32_t const b = (vertex.color >> 16) & 0xFFu;
        std::uint32_t const packed = (r << 16) | (g << 8) | b;
        float rgb = 0.0f;
        std::memcpy(&rgb, &packed, 4);
        appendFloat(buffer, rgb);

        appendFloat(buffer, vertex.normal.x);
        appendFloat(buffer, vertex.normal.y);
        appendFloat(buffer, vertex.normal.z);

        if (buffer.size() >= kBatchPoints * kPointBytes)
        {
            flush();
        }
    }
    flush();

    file.flush();
    if (!file)
    {
        throw std::runtime_error("写入 PCD 顶点数据失败（磁盘空间不足？）。");
    }
}

} // namespace app
