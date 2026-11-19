#pragma once

#include <algorithm>
#include <cmath>
#include <DirectXMath.h>

namespace app
{
    // 自由视角相机（右手坐标系，与 MeshLab / CloudCompare 等 PLY 工具一致）。
    //
    // 约定：
    //   - 世界坐标：Y 轴向上（可切换为 Z 轴向上）
    //   - 视线方向：Forward() = normalize(target - eye)
    //   - 左键拖动：只改变观察方向（相机位置不动）
    //   - 右键拖动：沿屏幕平面平移
    //   - 滚轮：沿视线方向前进 / 后退
    struct OrbitCamera
    {
        DirectX::XMFLOAT3 target{ 0.0f, 0.0f, 0.0f };
        float distance{ 3.0f };
        float yaw{ 0.65f };
        float pitch{ 0.35f };
        float fovY{ DirectX::XMConvertToRadians(45.0f) };
        float sceneRadius{ 1.0f };
        bool zUp{ false };

        // 交互速度倍率（界面“视角速度”滑杆，0.2 ~ 3.0，1.0 为默认）
        float moveSpeed{ 1.0f };

        static constexpr float kPitchLimit = 1.5533f; // ~89°
        static constexpr float kLookSensitivity = 0.008f;   // 弧度 / 像素
        static constexpr float kDollyFraction = 0.01f;      // 每个滚轮刻度移动当前距离的 1%
        static constexpr float kPanSpeed = 0.5f;            // 平移速度相对鼠标 1:1 的倍率

        // 相机的“右”方向（始终水平，由 yaw 决定）
        DirectX::XMFLOAT3 RightVector() const
        {
            if (zUp)
            {
                return { -std::sin(yaw), std::cos(yaw), 0.0f };
            }
            return { std::cos(yaw), 0.0f, -std::sin(yaw) };
        }

        // 相机的“上”方向：由 yaw/pitch 推导，与视线严格垂直。
        // 不能用世界坐标的 Z/Y 轴直接当作 up：俯仰接近 ±90°（例如从正上方看）时，
        // 世界 up 会与视线平行，LookAt 的叉乘退化，视图会突然翻转/打转。
        DirectX::XMFLOAT3 UpVector() const
        {
            DirectX::XMFLOAT3 const forward = Forward();
            DirectX::XMFLOAT3 const right = RightVector();
            return {
                right.y * forward.z - right.z * forward.y,
                right.z * forward.x - right.x * forward.z,
                right.x * forward.y - right.y * forward.x };
        }

        DirectX::XMFLOAT3 Eye() const
        {
            float const cp = std::cos(pitch);
            float const sp = std::sin(pitch);
            if (zUp)
            {
                return {
                    target.x + distance * cp * std::cos(yaw),
                    target.y + distance * cp * std::sin(yaw),
                    target.z + distance * sp };
            }
            return {
                target.x + distance * cp * std::sin(yaw),
                target.y + distance * sp,
                target.z + distance * cp * std::cos(yaw) };
        }

        // 由 yaw/pitch 计算单位视线方向（从相机指向目标）
        DirectX::XMFLOAT3 Forward() const
        {
            float const cp = std::cos(pitch);
            float const sp = std::sin(pitch);
            if (zUp)
            {
                return { -cp * std::cos(yaw), -cp * std::sin(yaw), -sp };
            }
            return { -cp * std::sin(yaw), -sp, -cp * std::cos(yaw) };
        }

        void Frame(DirectX::XMFLOAT3 const& boundsMin, DirectX::XMFLOAT3 const& boundsMax)
        {
            DirectX::XMFLOAT3 const center{
                (boundsMin.x + boundsMax.x) * 0.5f,
                (boundsMin.y + boundsMax.y) * 0.5f,
                (boundsMin.z + boundsMax.z) * 0.5f };

            float const dx = boundsMax.x - boundsMin.x;
            float const dy = boundsMax.y - boundsMin.y;
            float const dz = boundsMax.z - boundsMin.z;
            float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(radius) || radius < 1.0e-5f) radius = 1.0f;

            sceneRadius = radius;
            target = center;
            distance = radius / std::sin(fovY * 0.5f) * 1.1f;
            if (zUp)
            {
                // 默认正视图（略微俯视）：X 轴朝屏幕右侧，Z 轴朝上
                yaw = -DirectX::XM_PI * 0.5f;
                pitch = 0.30f;
            }
            else
            {
                yaw = 0.65f;
                pitch = 0.35f;
            }
        }

        // 左键拖动：只旋转观察方向，相机位置保持不变。
        // 水平方向：鼠标左移 -> 视角左转，鼠标右移 -> 视角右转；
        // 垂直方向：鼠标上移 -> 视角上转，鼠标下移 -> 视角下转
        // （由调用方传入已取反的 dx/dy）。
        void Look(float deltaXPixels, float deltaYPixels)
        {
            DirectX::XMFLOAT3 const eye = Eye();
            float const speed = std::clamp(moveSpeed, 0.05f, 10.0f);

            yaw += deltaXPixels * kLookSensitivity * speed;
            pitch -= deltaYPixels * kLookSensitivity * speed;
            pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);
            while (yaw > DirectX::XM_PI) yaw -= DirectX::XM_2PI;
            while (yaw < -DirectX::XM_PI) yaw += DirectX::XM_2PI;

            DirectX::XMFLOAT3 const forward = Forward();
            target = {
                eye.x + forward.x * distance,
                eye.y + forward.y * distance,
                eye.z + forward.z * distance };
        }

        // 右键拖动：沿屏幕平面平移，点云跟随鼠标移动。
        // 速度取 1:1 跟随鼠标的一半（kPanSpeed），并由“视角速度”滑杆再缩放。
        // viewportHeight 与 delta 使用相同单位（这里都是 DIP）。
        void Pan(float deltaXPixels, float deltaYPixels, float viewportHeight)
        {
            if (!(viewportHeight > 0.0f)) return;

            float const speed = std::clamp(moveSpeed, 0.05f, 10.0f);
            float const worldPerPixel =
                2.0f * distance * std::tan(fovY * 0.5f) / viewportHeight * kPanSpeed * speed;

            DirectX::XMFLOAT3 const eye = Eye();
            DirectX::XMFLOAT3 const upVector = UpVector();
            DirectX::XMVECTOR const forward = DirectX::XMVector3Normalize(
                DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&target), DirectX::XMLoadFloat3(&eye)));
            DirectX::XMVECTOR const up = DirectX::XMLoadFloat3(&upVector);
            // 右手系：right = forward × up
            DirectX::XMVECTOR const right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(forward, up));
            DirectX::XMVECTOR const cameraUp = DirectX::XMVector3Cross(right, forward);

            DirectX::XMVECTOR moved = DirectX::XMLoadFloat3(&target);
            moved = DirectX::XMVectorAdd(moved, DirectX::XMVectorScale(right, -deltaXPixels * worldPerPixel));
            moved = DirectX::XMVectorAdd(moved, DirectX::XMVectorScale(cameraUp, deltaYPixels * worldPerPixel));
            DirectX::XMStoreFloat3(&target, moved);
        }

        // 滚轮：沿视线方向前进 / 后退（不是缩放，相机与目标一起移动）。
        // 默认每个刻度移动当前距离的 1%（kDollyFraction），可通过 moveSpeed 调节。
        void Move(float wheelDelta)
        {
            float const steps = wheelDelta / 120.0f;
            float const speed = std::clamp(moveSpeed, 0.05f, 10.0f);
            float const amount = distance * kDollyFraction * speed * steps;
            if (!std::isfinite(amount)) return;

            DirectX::XMFLOAT3 const forward = Forward();
            target = {
                target.x + forward.x * amount,
                target.y + forward.y * amount,
                target.z + forward.z * amount };
        }

        // 键盘持续移动：沿视线前进（正）后退（负），世界单位。
        void MoveForward(float amount)
        {
            if (!std::isfinite(amount)) return;
            DirectX::XMFLOAT3 const forward = Forward();
            target = {
                target.x + forward.x * amount,
                target.y + forward.y * amount,
                target.z + forward.z * amount };
        }

        // 键盘持续移动：沿相机右方向平移（正 = 屏幕右移），世界单位。
        void Strafe(float amount)
        {
            if (!std::isfinite(amount)) return;
            DirectX::XMFLOAT3 const right = RightVector();
            target = {
                target.x + right.x * amount,
                target.y + right.y * amount,
                target.z + right.z * amount };
        }

        // 键盘持续移动：沿相机上方向平移（正 = 屏幕上移），世界单位。
        void MoveUp(float amount)
        {
            if (!std::isfinite(amount)) return;
            DirectX::XMFLOAT3 const up = UpVector();
            target = {
                target.x + up.x * amount,
                target.y + up.y * amount,
                target.z + up.z * amount };
        }

        DirectX::XMMATRIX View() const
        {
            DirectX::XMFLOAT3 const eye = Eye();
            DirectX::XMFLOAT3 const upVector = UpVector();
            return DirectX::XMMatrixLookAtRH(
                DirectX::XMLoadFloat3(&eye),
                DirectX::XMLoadFloat3(&target),
                DirectX::XMLoadFloat3(&upVector));
        }

        DirectX::XMMATRIX Projection(float aspectRatio) const
        {
            float nearPlane = std::max(distance * 0.002f, sceneRadius * 5.0e-5f);
            float farPlane = distance + sceneRadius * 8.0f;
            if (farPlane < nearPlane * 16.0f) farPlane = nearPlane * 16.0f;
            return DirectX::XMMatrixPerspectiveFovRH(fovY, std::max(aspectRatio, 0.01f), nearPlane, farPlane);
        }
    };
}
