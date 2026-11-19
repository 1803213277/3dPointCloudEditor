#include <filesystem>

#include "pch.h"
#include "App.xaml.h"
#include "AppContext.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace winrt::PointCloudEditor::implementation
{
    /// <summary>
    /// Initializes the singleton application object.  This is the first line of authored code
    /// executed, and as such is the logical equivalent of main() or WinMain().
    /// </summary>
    App::App()
    {
        // Xaml objects should not call InitializeComponent during construction.
        // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // 默认界面语言：跟随系统 UI 语言（中文系统用中文，其它用英文）
        {
            LANGID const languageId = ::GetUserDefaultUILanguage();
            app::SetLanguage(PRIMARYLANGID(languageId) == LANG_CHINESE
                ? app::Language::Chinese
                : app::Language::English);
        }

        window = make<MainWindow>();

        // 文件选择器（IInitializeWithWindow）需要窗口 HWND
        try
        {
            auto const windowNative = window.as<::IWindowNative>();
            HWND handle = nullptr;
            if (SUCCEEDED(windowNative->get_WindowHandle(&handle)))
            {
                app::MainWindowHandle = handle;
            }
        }
        catch (...)
        {
        }

        window.Activate();
        window.Title(L"3D点云编辑器");

        // 标题栏 / 任务栏图标：使用 exe 同目录下的 app.ico（发布目录里会带上）
        if (auto const appWindow = window.AppWindow())
        {
            try
            {
                wchar_t modulePath[MAX_PATH]{};
                if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
                {
                    std::filesystem::path const iconPath =
                        std::filesystem::path(modulePath).parent_path() / L"app.ico";
                    if (std::filesystem::exists(iconPath))
                    {
                        appWindow.SetIcon(iconPath.wstring());
                    }
                }
            }
            catch (...)
            {
            }
        }

        // 默认最大化（全屏工作区）；如果拿不到 OverlappedPresenter 就退回到固定尺寸
        if (auto const appWindow = window.AppWindow())
        {
            bool maximized = false;
            if (auto const presenter = appWindow.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                presenter.Maximize();
                maximized = true;
            }
            if (!maximized)
            {
                winrt::Windows::Graphics::SizeInt32 const size{ 1440, 960 };
                appWindow.Resize(size);
            }
        }
    }
}
