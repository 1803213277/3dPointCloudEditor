#pragma once

#include <cmath>
#include <cstddef>
#include <vector>
#include <DirectXMath.h>

namespace app
{
    // 多边形选择体的坐标系：
    //   多边形位于 origin + right*x + up*y 的平面上（局部坐标 x/y），
    //   沿 forward 方向从平面（深度 0）向场景内延伸 length，形成“立体多边形”棱柱。
    struct SelectionFrame
    {
        DirectX::XMFLOAT3 origin{};
        DirectX::XMFLOAT3 right{ 1.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT3 forward{ 0.0f, 0.0f, 1.0f };
    };

    // 多边形内部判定（射线法，边界附近的点按“内部”处理）
    inline bool PointInsidePolygon(
        DirectX::XMFLOAT2 const& point,
        std::vector<DirectX::XMFLOAT2> const& polygon)
    {
        std::size_t const count = polygon.size();
        if (count < 3)
        {
            return false;
        }

        bool inside = false;
        for (std::size_t i = 0, j = count - 1; i < count; j = i++)
        {
            DirectX::XMFLOAT2 const& a = polygon[j];
            DirectX::XMFLOAT2 const& b = polygon[i];
            if (((b.y > point.y) != (a.y > point.y)) &&
                (point.x < (a.x - b.x) * (point.y - b.y) / (a.y - b.y) + b.x))
            {
                inside = !inside;
            }
        }
        return inside;
    }

    inline bool IsPointInsidePrism(
        DirectX::XMFLOAT3 const& point,
        SelectionFrame const& frame,
        std::vector<DirectX::XMFLOAT2> const& polygon,
        float length)
    {
        using namespace DirectX;

        XMVECTOR const offset = XMVectorSubtract(XMLoadFloat3(&point), XMLoadFloat3(&frame.origin));
        float const x = XMVectorGetX(XMVector3Dot(offset, XMLoadFloat3(&frame.right)));
        float const y = XMVectorGetX(XMVector3Dot(offset, XMLoadFloat3(&frame.up)));
        float const z = XMVectorGetX(XMVector3Dot(offset, XMLoadFloat3(&frame.forward)));

        // 从多边形平面沿视线方向向场景内延伸 [0, length]
        if (z < 0.0f || z > length)
        {
            return false;
        }
        return PointInsidePolygon({ x, y }, polygon);
    }

    // 把屏幕坐标（DIP，左上角原点，y 向下）投射到选择平面上，得到多边形的局部 2D 坐标。
    inline bool ProjectScreenToSelectionPlane(
        float cursorX,
        float cursorY,
        float viewportWidth,
        float viewportHeight,
        float fovY,
        DirectX::XMFLOAT3 const& eye,
        DirectX::XMFLOAT3 const& right,
        DirectX::XMFLOAT3 const& up,
        DirectX::XMFLOAT3 const& forward,
        DirectX::XMFLOAT3 const& planeOrigin,
        DirectX::XMFLOAT2& local)
    {
        using namespace DirectX;

        if (!(viewportWidth > 1.0f) || !(viewportHeight > 1.0f))
        {
            return false;
        }

        float const ndcX = cursorX / viewportWidth * 2.0f - 1.0f;
        float const ndcY = 1.0f - cursorY / viewportHeight * 2.0f;
        float const tanHalf = std::tan(fovY * 0.5f);
        float const aspect = viewportWidth / viewportHeight;

        XMVECTOR const direction = XMVector3Normalize(XMVectorAdd(
            XMVectorAdd(
                XMLoadFloat3(&forward),
                XMVectorScale(XMLoadFloat3(&right), ndcX * tanHalf * aspect)),
            XMVectorScale(XMLoadFloat3(&up), ndcY * tanHalf)));

        float const denominator = XMVectorGetX(XMVector3Dot(direction, XMLoadFloat3(&forward)));
        if (std::fabs(denominator) < 1.0e-5f)
        {
            return false;
        }

        XMVECTOR const toPlane = XMVectorSubtract(XMLoadFloat3(&planeOrigin), XMLoadFloat3(&eye));
        float const distance = XMVectorGetX(XMVector3Dot(toPlane, XMLoadFloat3(&forward))) / denominator;
        if (!(distance > 0.0f))
        {
            return false;
        }

        XMVECTOR const hit = XMVectorAdd(XMLoadFloat3(&eye), XMVectorScale(direction, distance));
        XMVECTOR const offset = XMVectorSubtract(hit, XMLoadFloat3(&planeOrigin));
        local = {
            XMVectorGetX(XMVector3Dot(offset, XMLoadFloat3(&right))),
            XMVectorGetX(XMVector3Dot(offset, XMLoadFloat3(&up))) };
        return true;
    }
}
