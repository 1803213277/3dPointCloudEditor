// 相机/交互方向自检：验证手性、旋转方向、平移方向、滚轮前进方向、选择几何。
// 编译并运行：见 tests/build-test.ps1（也可单独：cl /std:c++20 /EHsc /O2 /I"..\3dPointCloudEditor" CameraTest.cpp）
#include <cmath>
#include <cstdio>
#include <DirectXMath.h>

#include "OrbitCamera.h"
#include "SelectionVolume.h"

namespace
{
    int g_failures = 0;

    void Check(bool condition, char const* what)
    {
        std::printf("  [%s] %s\n", condition ? " ok " : "FAIL", what);
        if (!condition) ++g_failures;
    }

    DirectX::XMFLOAT3 Sub(DirectX::XMFLOAT3 const& a, DirectX::XMFLOAT3 const& b)
    {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    float Dot(DirectX::XMFLOAT3 const& a, DirectX::XMFLOAT3 const& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    bool Project(DirectX::XMFLOAT4X4 const& viewProjection, DirectX::XMFLOAT3 const& point, DirectX::XMFLOAT3& ndc)
    {
        DirectX::XMVECTOR const p = DirectX::XMVectorSet(point.x, point.y, point.z, 1.0f);
        DirectX::XMVECTOR const clip = DirectX::XMVector4Transform(p, DirectX::XMLoadFloat4x4(&viewProjection));
        float const w = DirectX::XMVectorGetW(clip);
        if (w <= 1.0e-6f) return false;
        ndc = {
            DirectX::XMVectorGetX(clip) / w,
            DirectX::XMVectorGetY(clip) / w,
            DirectX::XMVectorGetZ(clip) / w };
        return true;
    }
}

int wmain()
{
    app::OrbitCamera camera;
    camera.Frame({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });

    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMStoreFloat4x4(
        &viewProjection,
        DirectX::XMMatrixMultiply(camera.View(), camera.Projection(16.0f / 9.0f)));

    // 1) 右手系：世界 +X 应出现在屏幕右侧（ndc.x > 0），与 MeshLab 等一致
    DirectX::XMFLOAT3 ndc{};
    bool const projected = Project(viewProjection, { 1.0f, 0.0f, 0.0f }, ndc);
    Check(projected, "+X is in front of the camera");
    Check(projected && ndc.x > 0.05f, "+X projects to the RIGHT side of the screen");
    Check(projected && ndc.y > -0.2f && ndc.y < 0.2f, "+X is roughly on the horizon line");

    // 2) 左键拖动：拖动方向 = 视角方向（左移 -> 视角左转，右移 -> 视角右转），相机位置不变
    DirectX::XMFLOAT3 const eyeBefore = camera.Eye();
    DirectX::XMFLOAT3 const forwardBefore = camera.Forward();
    auto dragLook = [](app::OrbitCamera& cam, float dragX, float dragY)
    {
        // 与 MainWindow::OnPointerMoved 一致：水平/垂直方向都取反
        cam.Look(-dragX, -dragY);
    };
    dragLook(camera, -100.0f, 0.0f);
    DirectX::XMFLOAT3 const eyeAfterLook = camera.Eye();
    DirectX::XMFLOAT3 const forwardAfter = camera.Forward();
    Check(std::fabs(eyeAfterLook.x - eyeBefore.x) < 1e-4f &&
          std::fabs(eyeAfterLook.y - eyeBefore.y) < 1e-4f &&
          std::fabs(eyeAfterLook.z - eyeBefore.z) < 1e-4f,
          "left drag (look) keeps the camera position");
    Check(forwardAfter.x < forwardBefore.x, "dragging left turns the view to the left (-X)");
    Check(camera.target.x == camera.Eye().x + camera.Forward().x * camera.distance ||
          std::fabs(camera.target.x - (camera.Eye().x + camera.Forward().x * camera.distance)) < 1e-3f,
          "target stays in front of the camera");

    DirectX::XMFLOAT3 const forwardBeforeRightDrag = camera.Forward();
    dragLook(camera, 100.0f, 0.0f);
    Check(camera.Forward().x > forwardBeforeRightDrag.x, "dragging right turns the view to the right (+X)");

    // 3) 上下方向也跟拖动方向一致：上拖 -> 视角上转，下拖 -> 视角下转
    DirectX::XMFLOAT3 const forwardUp0 = camera.Forward();
    dragLook(camera, 0.0f, -80.0f);
    Check(camera.Forward().y > forwardUp0.y, "dragging up turns the view upward");

    DirectX::XMFLOAT3 const forwardDown0 = camera.Forward();
    dragLook(camera, 0.0f, 160.0f);
    Check(camera.Forward().y < forwardDown0.y, "dragging down turns the view downward");

    // 4) 滚轮向上 -> 沿视线前进
    DirectX::XMFLOAT3 const eyeBeforeMove = camera.Eye();
    DirectX::XMFLOAT3 const forwardBeforeMove = camera.Forward();
    camera.Move(120.0f);
    DirectX::XMFLOAT3 const moved = Sub(camera.Eye(), eyeBeforeMove);
    float const movedAlongForward = Dot(moved, forwardBeforeMove);
    Check(movedAlongForward > 0.0f, "wheel up moves the camera forward along the view direction");
    Check(std::fabs(Dot(moved, { forwardBeforeMove.y, -forwardBeforeMove.x, 0.0f })) < 1e-2f,
          "wheel movement has no sideways component");

    // 5) 右键右拖 -> 相机左移（场景跟随鼠标右移）
    DirectX::XMFLOAT3 const eyeBeforePan = camera.Eye();
    DirectX::XMFLOAT3 const forwardPan = camera.Forward();
    DirectX::XMVECTOR const right = DirectX::XMVector3Normalize(
        DirectX::XMVector3Cross(DirectX::XMLoadFloat3(&forwardPan), DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    DirectX::XMFLOAT3 rightVector{};
    DirectX::XMStoreFloat3(&rightVector, right);
    camera.Pan(100.0f, 0.0f, 900.0f);
    float const panAlongRight = Dot(Sub(camera.Eye(), eyeBeforePan), rightVector);
    Check(panAlongRight < 0.0f, "right drag moves the camera left (scene follows mouse)");

    float const panDistance = std::sqrt(Dot(Sub(camera.Eye(), eyeBeforePan), Sub(camera.Eye(), eyeBeforePan)));
    float const expectedPan =
        2.0f * camera.distance * std::tan(camera.fovY * 0.5f) / 900.0f * 100.0f * 0.5f;
    Check(std::fabs(panDistance - expectedPan) < 1e-3f,
          "pan speed is 0.5x of 1:1 mouse movement (slower than before)");

    // 6) 右键下拖 -> 相机上移（场景跟随鼠标下移）
    DirectX::XMFLOAT3 const eyeBeforePanY = camera.Eye();
    camera.Pan(0.0f, 100.0f, 900.0f);
    Check(camera.Eye().y > eyeBeforePanY.y, "drag down moves the camera up (scene follows mouse)");

    // 7) Z 轴向上模式：屏幕右方向与转视角方向仍然正确
    camera.zUp = true;
    camera.Frame({ -1.0f, -1.0f, -0.5f }, { 1.0f, 1.0f, 0.5f });

    DirectX::XMFLOAT3 const forwardZ = camera.Forward();
    DirectX::XMFLOAT3 const upZ = camera.UpVector();
    DirectX::XMVECTOR const rightZVector = DirectX::XMVector3Normalize(
        DirectX::XMVector3Cross(DirectX::XMLoadFloat3(&forwardZ), DirectX::XMLoadFloat3(&upZ)));
    DirectX::XMFLOAT3 rightZ{};
    DirectX::XMStoreFloat3(&rightZ, rightZVector);

    DirectX::XMFLOAT3 pointOnRight{
        camera.target.x + rightZ.x * 0.3f,
        camera.target.y + rightZ.y * 0.3f,
        camera.target.z + rightZ.z * 0.3f };

    DirectX::XMFLOAT4X4 viewProjectionZ{};
    DirectX::XMStoreFloat4x4(
        &viewProjectionZ,
        DirectX::XMMatrixMultiply(camera.View(), camera.Projection(16.0f / 9.0f)));

    DirectX::XMFLOAT3 ndcZ{};
    bool const projectedZ = Project(viewProjectionZ, pointOnRight, ndcZ);
    Check(projectedZ && ndcZ.x > 0.05f, "Z-up: camera right maps to the RIGHT side of the screen");

    DirectX::XMFLOAT3 const forwardZBefore = camera.Forward();
    camera.Look(100.0f, 0.0f); // 拖动左移（OnPointerMoved 取反后为正）
    Check(Dot(camera.Forward(), rightZ) < Dot(forwardZBefore, rightZ),
          "Z-up: dragging left turns the view to the left");
    camera.Look(-100.0f, 0.0f);

    DirectX::XMFLOAT3 const eyeZBeforeMove = camera.Eye();
    DirectX::XMFLOAT3 const forwardZBeforeMove = camera.Forward();
    camera.Move(120.0f);
    Check(Dot(Sub(camera.Eye(), eyeZBeforeMove), forwardZBeforeMove) > 0.0f,
          "Z-up: wheel up moves the camera forward");

    // 8) 视角速度倍率：滚轮步长与速度倍率成正比，默认每刻度 1% 距离
    camera.zUp = false;
    camera.moveSpeed = 1.0f;
    camera.Frame({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });

    DirectX::XMFLOAT3 const eyeSpeed0 = camera.Eye();
    float const distanceBefore = camera.distance;
    camera.Move(120.0f);
    float const movedDefault = std::sqrt(Dot(Sub(camera.Eye(), eyeSpeed0), Sub(camera.Eye(), eyeSpeed0)));
    Check(std::fabs(movedDefault - distanceBefore * 0.01f) < 1e-4f,
          "default wheel step is 1% of the camera distance");

    camera.Frame({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });
    camera.moveSpeed = 2.0f;
    DirectX::XMFLOAT3 const eyeSpeed2 = camera.Eye();
    camera.Move(120.0f);
    DirectX::XMFLOAT3 const deltaFast = Sub(camera.Eye(), eyeSpeed2);
    float const movedFast = std::sqrt(Dot(deltaFast, deltaFast));
    Check(std::fabs(movedFast - movedDefault * 2.0f) < 1e-3f,
          "speed 2.0 doubles the wheel step");

    // 视角旋转量与速度倍率成正比（用水平角比较，避免 sin/cos 非线性）
    auto yawOf = [](DirectX::XMFLOAT3 const& forward) { return std::atan2(-forward.x, -forward.z); };

    camera.Frame({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });
    camera.moveSpeed = 1.0f;
    float const yawSlowBefore = yawOf(camera.Forward());
    camera.Look(10.0f, 0.0f);
    float const slowTurn = std::fabs(yawOf(camera.Forward()) - yawSlowBefore);

    camera.Frame({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });
    camera.moveSpeed = 2.0f;
    float const yawFastBefore = yawOf(camera.Forward());
    camera.Look(10.0f, 0.0f);
    float const fastTurn = std::fabs(yawOf(camera.Forward()) - yawFastBefore);

    Check(std::fabs(fastTurn - slowTurn * 2.0f) < 1e-4f, "speed 2.0 doubles the look rotation");

    // 9) 多边形选择体：点在多边形内 + 深度范围内
    std::vector<DirectX::XMFLOAT2> const square = {
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

    Check(app::PointInsidePolygon({ 0.0f, 0.0f }, square), "polygon: center is inside");
    Check(app::PointInsidePolygon({ 0.9f, 0.9f }, square), "polygon: near corner is inside");
    Check(!app::PointInsidePolygon({ 1.5f, 0.0f }, square), "polygon: outside point is outside");

    app::SelectionFrame prismFrame{};
    prismFrame.origin = { 1.0f, 2.0f, 3.0f };
    prismFrame.right = { 1.0f, 0.0f, 0.0f };
    prismFrame.up = { 0.0f, 1.0f, 0.0f };
    prismFrame.forward = { 0.0f, 0.0f, 1.0f };

    Check(app::IsPointInsidePrism({ 1.0f, 2.0f, 3.0f }, prismFrame, square, 0.5f),
          "prism: center is inside");
    Check(app::IsPointInsidePrism({ 1.9f, 2.9f, 3.4f }, prismFrame, square, 0.5f),
          "prism: point inside polygon and depth is inside");
    Check(!app::IsPointInsidePrism({ 1.0f, 2.0f, 3.6f }, prismFrame, square, 0.5f),
          "prism: point beyond length is outside");
    Check(!app::IsPointInsidePrism({ 1.0f, 2.0f, 2.9f }, prismFrame, square, 0.5f),
          "prism: point behind the polygon plane is outside");
    Check(!app::IsPointInsidePrism({ 2.2f, 2.0f, 3.0f }, prismFrame, square, 0.5f),
          "prism: point outside polygon is outside");

    // 10) 屏幕坐标 -> 选择平面
    app::OrbitCamera planeCamera;
    planeCamera.zUp = true;
    planeCamera.Frame({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });
    {
        constexpr float kWidth = 1440.0f;
        constexpr float kHeight = 900.0f;
        DirectX::XMFLOAT3 const eye = planeCamera.Eye();
        DirectX::XMFLOAT3 const right = planeCamera.RightVector();
        DirectX::XMFLOAT3 const up = planeCamera.UpVector();
        DirectX::XMFLOAT3 const forward = planeCamera.Forward();
        DirectX::XMFLOAT3 const origin = planeCamera.target;

        DirectX::XMFLOAT2 centerLocal{};
        Check(app::ProjectScreenToSelectionPlane(
                  kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, planeCamera.fovY,
                  eye, right, up, forward, origin, centerLocal),
              "selection plane: screen center projects onto the plane");
        Check(std::fabs(centerLocal.x) < 0.01f && std::fabs(centerLocal.y) < 0.01f,
              "selection plane: screen center is the plane origin");

        DirectX::XMFLOAT2 offsetLocal{};
        Check(app::ProjectScreenToSelectionPlane(
                  kWidth * 0.5f + 200.0f, kHeight * 0.5f, kWidth, kHeight, planeCamera.fovY,
                  eye, right, up, forward, origin, offsetLocal),
              "selection plane: offset cursor projects onto the plane");
        Check(offsetLocal.x > 0.0f, "selection plane: cursor to the right gives positive local x");
    }

    // 12) 俯仰到接近 ±90°（例如 Z-up 从正上方看）时，视图不能翻转或出现 NaN
    {
        auto viewIsFinite = [](app::OrbitCamera const& camera)
        {
            DirectX::XMFLOAT4X4 matrix{};
            DirectX::XMStoreFloat4x4(&matrix, camera.View());
            float const* values = &matrix._11;
            for (int index = 0; index < 16; ++index)
            {
                if (!std::isfinite(values[index])) return false;
            }
            return true;
        };

        app::OrbitCamera poleCamera;
        poleCamera.zUp = true;
        poleCamera.Frame({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });

        poleCamera.Look(0.0f, 1.0e6f); // 一直往上拉，pitch 会被夹到 +89°
        DirectX::XMFLOAT3 const rightTop = poleCamera.RightVector();
        float const rightLength = std::sqrt(Dot(rightTop, rightTop));
        Check(std::isfinite(rightLength) && std::fabs(rightLength - 1.0f) < 1.0e-3f,
              "pole: right vector stays unit length at +89 pitch");
        Check(viewIsFinite(poleCamera), "pole: view matrix is finite at +89 pitch");

        poleCamera.Look(0.0f, -2.0e6f); // 反向拉到底，pitch = -89°
        Check(viewIsFinite(poleCamera), "pole: view matrix is finite at -89 pitch");

        poleCamera.Look(400.0f, 0.0f); // 在极点附近水平转视角
        Check(viewIsFinite(poleCamera), "pole: view matrix is finite while rotating near the pole");
    }

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "CAMERA TESTS PASSED" : "CAMERA TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
