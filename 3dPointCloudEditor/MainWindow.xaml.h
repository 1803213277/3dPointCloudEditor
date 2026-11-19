#pragma once

#include "MainWindow.g.h"

#include <cstdint>
#include <memory>
#include <string>

#include <winrt/Windows.UI.ViewManagement.h>

#include "OrbitCamera.h"
#include "PlyLoader.h"
#include "PointCloudRenderer.h"
#include "Localization.h"

namespace winrt::PointCloudEditor::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        int32_t MyProperty();
        void MyProperty(int32_t value);

        // 顶部菜单（XAML 里 Click 绑定的处理函数必须是 public）
        void OnMenuOpenClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuSaveClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuSavePcdClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuExitClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuSelectModeClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuDeletePointsClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuClearSelectionClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuResetViewClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnMenuAboutClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        // 右侧视图切换
        void OnViewTopClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnViewBottomClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnViewFrontClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnViewBackClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnViewLeftClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnViewRightClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        // 坐标调整：平移 / 旋转
        void OnTransformXMinusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformXPlusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformYMinusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformYPlusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformZMinusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformZPlusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformRXMinusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformRXPlusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformRYMinusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformRYPlusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformRZMinusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTransformRZPlusClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        // 键盘移动（W/S 前进后退，A/D 左移右移）
        void OnLanguageChineseClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLanguageEnglishClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnThemeSystemClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnThemeLightClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnThemeDarkClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnAxesToggleClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnViewportKeyDown(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
        void OnViewportKeyUp(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
        void OnViewportLostFocus(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        winrt::fire_and_forget ShowAboutAsync();

        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_moveTimer{ nullptr };
        bool m_keyW{ false };
        bool m_keyA{ false };
        bool m_keyS{ false };
        bool m_keyD{ false };
        bool m_keyQ{ false };
        bool m_keyE{ false };
        void OnMoveTick();
        void StopKeyboardMove();

        struct CloudTransform
        {
            float offsetX{ 0.0f };
            float offsetY{ 0.0f };
            float offsetZ{ 0.0f };
            float rotX{ 0.0f };
            float rotY{ 0.0f };
            float rotZ{ 0.0f };
        };
        CloudTransform m_activeTransform{};
        float TranslationStep();
        float RotationStep();
        void ApplyTranslation(float dx, float dy, float dz);
        void ApplyRotation(float rx, float ry, float rz);
        void UpdateTransformLabels();

        // 多点点云：m_cloud 为当前点云，其余存放在 m_otherClouds
        struct StoredCloud
        {
            std::uint64_t id{ 0 };
            std::wstring name;
            app::PlyCloud cloud;
            bool visible{ true };
            CloudTransform transform{};
        };
        std::vector<StoredCloud> m_otherClouds;
        std::vector<std::uint64_t> m_cloudOrder; // 列表显示顺序（按加载顺序，不随选中变化）
        std::uint64_t m_activeCloudId{ 0 };
        std::uint64_t m_nextCloudId{ 1 };
        bool m_activeCloudVisible{ true };
        bool m_updatingCloudList{ false };
        void UpdateCloudList();
        void ActivateCloudById(std::uint64_t id);
        void SetCloudVisible(std::uint64_t id, bool visible);
        void RebuildVisibleClouds();
        void ApplyViewPreset(float yaw, float pitch);

        enum class DragMode
        {
            None,
            Look,
            Pan,
        };

        void InitializeScene();
        void HookEvents();

        void OpenFile();
        void LoadDemo();
        // 从 exe 同目录的 samples\ 里打开内置示例点云；文件缺失时退回程序内置的示例。
        void LoadSampleCloud(std::wstring const& fileName, std::wstring const& displayName);
        void ResetView();
        void ApplySpeedSetting();

        // 点云聚类选择
        void OnClusterToolToggled();
        void ApplyClusterRadius();
        void ApplyClusterRange();
        void SelectClusterAt(float cursorX, float cursorY);
        void DeleteClusterPoints();
        void BuildClusterFromSeed(std::uint32_t seedIndex);
        bool TryGetClusterSeed(float cursorX, float cursorY, std::uint32_t& seedIndex);
        void OnBoundaryChanged();
        void ConfigureBoundarySliders(
            DirectX::XMFLOAT3 const& min,
            DirectX::XMFLOAT3 const& max);
        void ApplyBoundaryFilter();

        void OnPointerPressed(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPointerMoved(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPointerReleased(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPointerCaptureLost(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPointerWheelChanged(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnDragOver(winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void OnRenderTick();
        void UpdateAxisGizmo(DirectX::XMMATRIX const& view);

        winrt::fire_and_forget PickFileAsync();
        winrt::fire_and_forget SaveCloudAsync(bool pcd);
        winrt::fire_and_forget HandleDropAsync(winrt::Microsoft::UI::Xaml::DragEventArgs args);
        winrt::fire_and_forget LoadStartupFileAsync(std::wstring path);
        winrt::Windows::Foundation::IAsyncAction LoadFileCoreAsync(std::wstring path);
        winrt::Windows::Foundation::IAsyncAction LoadFileCoreAsync(std::wstring path, std::wstring displayName);

        void ApplyCloud(app::PlyCloud cloud, std::wstring displayName);
        void RequestRender();
        void RenderFrame();
        void UpdateStats();
        void SetBusy(bool busy);
        void SetStatus(std::wstring const& text);
        void ShowError(std::wstring const& message);

        std::unique_ptr<app::PointCloudRenderer> m_renderer;
        app::OrbitCamera m_camera;
        app::PlyCloud m_cloud;
        std::wstring m_displayName;

        // 点云聚类选择：点击点云取一个种子点，选中与它连通的一片点
        bool m_clusterToolActive{ false };
        bool m_clusterSelected{ false };
        bool m_clusterPendingClick{ false };
        float m_clusterPressX{ 0.0f };
        float m_clusterPressY{ 0.0f };
        std::uint32_t m_clusterSeedIndex{ 0 };
        std::vector<std::uint32_t> m_clusterIndices;
        std::vector<std::uint32_t> m_selectedIndices;
        DirectX::XMFLOAT3 m_boundaryMin{};
        DirectX::XMFLOAT3 m_boundaryMax{};
        bool m_updatingBoundarySliders{ false };
        float m_clusterRadius{ 0.1f };
        float m_clusterRange{ 3.0f };

        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_renderTimer{ nullptr };

        winrt::Windows::UI::ViewManagement::UISettings m_uiSettings{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };
        winrt::event_token m_themeChangedToken{};
        bool m_axesVisible{ true };
        enum class ThemePreference
        {
            System,
            Light,
            Dark,
        };
        ThemePreference m_themePreference{ ThemePreference::System };
        void ApplyTheme();
        void ApplyLanguage();

        DragMode m_dragMode{ DragMode::None };
        std::uint32_t m_dragPointerId{ 0 };
        float m_lastPointerX{ 0.0f };
        float m_lastPointerY{ 0.0f };
        bool m_initialized{ false };
        bool m_busy{ false };
        bool m_renderFailureReported{ false };
    };
}

namespace winrt::PointCloudEditor::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
