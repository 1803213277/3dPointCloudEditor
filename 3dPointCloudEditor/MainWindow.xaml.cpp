#include "pch.h"
#include "MainWindow.xaml.h"
#include "AppContext.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <winrt/Windows.UI.Text.h>

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <cwchar>
#include <cwctype>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    // winrt::resume_foreground 只提供了 Windows.System.DispatcherQueue 的重载，
    // 这里为 WinUI 3 使用的 Microsoft.UI.Dispatching.DispatcherQueue 实现等价 awaiter。
    struct DispatcherQueueAwaiter
    {
        winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher;

        bool await_ready() const noexcept
        {
            return dispatcher.HasThreadAccess();
        }

        void await_suspend(std::coroutine_handle<> handle) const
        {
            if (!dispatcher.TryEnqueue([handle]() { handle.resume(); }))
            {
                throw winrt::hresult_error(E_FAIL, L"无法把后续操作调度到 UI 线程。");
            }
        }

        void await_resume() const noexcept
        {
        }
    };

    DispatcherQueueAwaiter ResumeOnDispatcher(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher)
    {
        return DispatcherQueueAwaiter{ dispatcher };
    }

    std::wstring Utf8ToWide(std::string const& text)
    {
        if (text.empty()) return {};
        int const length = ::MultiByteToWideChar(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        ::MultiByteToWideChar(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length);
        return result;
    }

    // exe 同目录下的 samples\ —— 内置示例点云放在这里（构建时由项目复制过来）。
    std::filesystem::path SampleCloudsDirectory()
    {
        wchar_t modulePath[MAX_PATH]{};
        if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
        {
            return std::filesystem::path(modulePath).parent_path() / L"samples";
        }
        return std::filesystem::path(L"samples");
    }

    std::wstring FormatCount(std::uint64_t value)
    {
        std::wstring const digits = std::to_wstring(value);
        std::wstring result;
        result.reserve(digits.size() + digits.size() / 3);
        int counter = 0;
        for (auto iterator = digits.rbegin(); iterator != digits.rend(); ++iterator)
        {
            if (counter > 0 && counter % 3 == 0) result.insert(result.begin(), L',');
            result.insert(result.begin(), *iterator);
            ++counter;
        }
        return result;
    }

    std::wstring FormatMeters(float value)
    {
        wchar_t buffer[32]{};
        std::swprintf(buffer, 32, L"%.2f m", value);
        return buffer;
    }

    std::wstring FormatBytes(std::uint64_t bytes)
    {
        wchar_t buffer[64]{};
        if (bytes >= 1024ull * 1024ull * 1024ull)
        {
            std::swprintf(buffer, 64, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        }
        else if (bytes >= 1024ull * 1024ull)
        {
            std::swprintf(buffer, 64, L"%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        else if (bytes >= 1024ull)
        {
            std::swprintf(buffer, 64, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
        }
        else
        {
            std::swprintf(buffer, 64, L"%llu B", static_cast<unsigned long long>(bytes));
        }
        return buffer;
    }

    // 使用 Win32 文件对话框（IFileOpenDialog / IFileSaveDialog）而不是 WinRT 的
    // FileOpenPicker / FileSavePicker：WinRT 选择器在“以管理员身份运行”的进程里
    // 会因为权限隔离返回“拒绝访问”，而 Win32 对话框在普通/提升进程里都能正常工作。
    // 让文件对话框默认打开“文档”文件夹，避免落在只读/受保护目录而出现“没有权限”提示。
    void SetDialogStartFolder(IFileDialog* dialog)
    {
        PWSTR folderPath = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &folderPath)))
        {
            winrt::com_ptr<IShellItem> folder;
            if (SUCCEEDED(::SHCreateItemFromParsingName(folderPath, nullptr, IID_PPV_ARGS(folder.put()))))
            {
                dialog->SetFolder(folder.get());
            }
            ::CoTaskMemFree(folderPath);
        }
    }

    std::optional<std::wstring> ShowOpenFileDialog(HWND owner)
    {
        winrt::com_ptr<IFileOpenDialog> dialog;
        winrt::check_hresult(::CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        SetDialogStartFolder(dialog.get());

        DWORD options = 0;
        winrt::check_hresult(dialog->GetOptions(&options));
        winrt::check_hresult(dialog->SetOptions(
            options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST));

        std::wstring const cloudFilter = app::L(L"点云文件 (*.ply;*.pcd)", L"Point Cloud (*.ply;*.pcd)");
        std::wstring const plyFilter = app::L(L"PLY 点云 (*.ply)", L"PLY point cloud (*.ply)");
        std::wstring const pcdFilter = app::L(L"PCD 点云 (*.pcd)", L"PCD point cloud (*.pcd)");
        std::wstring const allFilter = app::L(L"所有文件 (*.*)", L"All files (*.*)");

        COMDLG_FILTERSPEC const filters[] = {
            { cloudFilter.c_str(), L"*.ply;*.pcd" },
            { plyFilter.c_str(), L"*.ply" },
            { pcdFilter.c_str(), L"*.pcd" },
            { allFilter.c_str(), L"*.*" },
        };
        winrt::check_hresult(dialog->SetFileTypes(ARRAYSIZE(filters), filters));
        winrt::check_hresult(dialog->SetTitle(app::L(L"打开点云文件", L"Open Point Cloud").c_str()));

        HRESULT const result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
        winrt::check_hresult(result);

        winrt::com_ptr<IShellItem> item;
        winrt::check_hresult(dialog->GetResult(item.put()));
        PWSTR rawPath = nullptr;
        winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
        std::wstring path = rawPath ? rawPath : L"";
        ::CoTaskMemFree(rawPath);
        if (path.empty()) return std::nullopt;
        return path;
    }

    std::optional<std::wstring> ShowSaveFileDialog(HWND owner, std::wstring const& suggestedName, bool pcd)
    {
        winrt::com_ptr<IFileSaveDialog> dialog;
        winrt::check_hresult(::CoCreateInstance(
            CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        SetDialogStartFolder(dialog.get());

        DWORD options = 0;
        winrt::check_hresult(dialog->GetOptions(&options));
        winrt::check_hresult(dialog->SetOptions(
            options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST));

        std::wstring const primaryFilter = pcd ? app::L(L"PCD 点云 (*.pcd)", L"PCD point cloud (*.pcd)")
                                               : app::L(L"PLY 点云 (*.ply)", L"PLY point cloud (*.ply)");
        std::wstring const allFilter = app::L(L"所有文件 (*.*)", L"All files (*.*)");

        COMDLG_FILTERSPEC const filters[] = {
            { primaryFilter.c_str(), pcd ? L"*.pcd" : L"*.ply" },
            { allFilter.c_str(), L"*.*" },
        };
        winrt::check_hresult(dialog->SetFileTypes(ARRAYSIZE(filters), filters));
        winrt::check_hresult(dialog->SetFileTypeIndex(1));
        winrt::check_hresult(dialog->SetDefaultExtension(pcd ? L"pcd" : L"ply"));
        winrt::check_hresult(dialog->SetTitle(
            (pcd ? app::L(L"保存 PCD 文件", L"Save PCD File")
                 : app::L(L"保存 PLY 文件", L"Save PLY File")).c_str()));

        if (!suggestedName.empty())
        {
            winrt::check_hresult(dialog->SetFileName(suggestedName.c_str()));
        }

        HRESULT const result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
        winrt::check_hresult(result);

        winrt::com_ptr<IShellItem> item;
        winrt::check_hresult(dialog->GetResult(item.put()));
        PWSTR rawPath = nullptr;
        winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
        std::wstring path = rawPath ? rawPath : L"";
        ::CoTaskMemFree(rawPath);
        if (path.empty()) return std::nullopt;
        return path;
    }

    std::wstring ToLower(std::wstring value)    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character)
        {
            return static_cast<wchar_t>(::towlower(character));
        });
        return value;
    }

    bool EndsWith(std::wstring const& value, std::wstring const& suffix)
    {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::vector<std::wstring> GetCommandLineArguments()
    {
        std::vector<std::wstring> arguments;
        int count = 0;
        LPWSTR* const rawArguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
        if (!rawArguments) return arguments;

        for (int index = 0; index < count; ++index)
        {
            arguments.emplace_back(rawArguments[index]);
        }
        ::LocalFree(rawArguments);
        return arguments;
    }
} // namespace

namespace winrt::PointCloudEditor::implementation
{
    MainWindow::MainWindow()
    {
        // XAML 内容由 C++/WinRT 在构造之后（make<MainWindow>() 内部）通过
        // InitializeComponent 加载，因此这里不能直接访问命名控件。
        // 通过消息队列延后到下一次调度再初始化渲染器与事件。
        if (auto const dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread())
        {
            dispatcher.TryEnqueue([this]()
            {
                InitializeScene();
            });
        }
    }

    int32_t MainWindow::MyProperty()
    {
        return 0;
    }

    void MainWindow::MyProperty(int32_t /* value */)
    {
    }

    void MainWindow::InitializeScene()
    {
        if (m_initialized) return;
        m_initialized = true;

        // 点云数据（CloudCompare / PCL 等）通常以 Z 轴为上方向
        m_camera.zUp = true;

        HookEvents();
        ApplyLanguage();
        ApplySpeedSetting();
        ApplyClusterRadius();
        ApplyClusterRange();

        if (auto const dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread())
        {
            m_renderTimer = dispatcher.CreateTimer();
            m_renderTimer.Interval(std::chrono::milliseconds(16));
            m_renderTimer.IsRepeating(false);
            m_renderTimer.Tick([this](auto&&, auto&&)
            {
                OnRenderTick();
            });

            m_moveTimer = dispatcher.CreateTimer();
            m_moveTimer.Interval(std::chrono::milliseconds(16));
            m_moveTimer.IsRepeating(true);
            m_moveTimer.Tick([this](auto&&, auto&&)
            {
                OnMoveTick();
            });
        }

        // SwapChainPanel 必须先完成布局、拿到真实像素尺寸再创建交换链，
        // 否则会按 0 尺寸创建，导致整个窗口只有一片黑色。这里强制同步布局一次。
        RootGrid().UpdateLayout();

        try
        {
            m_renderer = std::make_unique<app::PointCloudRenderer>();
            m_renderer->Initialize(Viewport());
            SetStatus(app::L(L"DirectX 12 就绪 · 显示适配器：", L"DirectX 12 ready · adapter: ") + m_renderer->AdapterName());
        }
        catch (std::exception const& error)
        {
            ShowError(L"DirectX 12 初始化失败：" + Utf8ToWide(error.what()));
            return;
        }

        // 跟随系统浅色/深色主题，并在系统切换主题时实时更新
        m_dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        try
        {
            m_uiSettings = winrt::Windows::UI::ViewManagement::UISettings{};
            m_themeChangedToken = m_uiSettings.ColorValuesChanged([this](auto&&, auto&&)
            {
                if (m_dispatcher)
                {
                    m_dispatcher.TryEnqueue([this]() { ApplyTheme(); });
                }
            });
        }
        catch (...)
        {
        }
        ApplyTheme();

        RequestRender();

        // 支持命令行直接打开：3dPointCloudEditor.exe 模型.ply / 模型.pcd  或  3dPointCloudEditor.exe --demo
        auto const arguments = GetCommandLineArguments();
        for (std::size_t index = 1; index < arguments.size(); ++index)
        {
            std::wstring const& argument = arguments[index];
            if (argument == L"--demo" || argument == L"-d")
            {
                LoadDemo();
                break;
            }
            if (EndsWith(ToLower(argument), L".ply") || EndsWith(ToLower(argument), L".pcd"))
            {
                LoadStartupFileAsync(argument);
                break;
            }
        }
    }

    void MainWindow::HookEvents()
    {
        OpenButton().Click([this](auto&&, auto&&)
        {
            OpenFile();
        });

        // 「示例点云」下拉菜单：四个内置示例对应 samples\ 里的四个文件
        DemoSphereItem().Click([this](auto&&, auto&&)
        {
            LoadSampleCloud(L"sphere_ascii.ply", app::L(L"示例：球体", L"Sample: Sphere"));
        });
        DemoWaveItem().Click([this](auto&&, auto&&)
        {
            LoadSampleCloud(L"wave_binary_le.ply", app::L(L"示例：波浪曲面", L"Sample: Wave"));
        });
        DemoHelixItem().Click([this](auto&&, auto&&)
        {
            LoadSampleCloud(L"helix_binary_be.ply", app::L(L"示例：螺旋线", L"Sample: Helix"));
        });
        DemoTorusItem().Click([this](auto&&, auto&&)
        {
            LoadSampleCloud(L"torus.pcd", app::L(L"示例：环面", L"Sample: Torus"));
        });

        ResetButton().Click([this](auto&&, auto&&)
        {
            ResetView();
        });

        PointSizeSlider().ValueChanged([this](auto&&, auto&&)
        {
            RequestRender();
        });

        SpeedSlider().ValueChanged([this](auto&&, auto&&)
        {
            ApplySpeedSetting();
        });

        ClusterRadiusSlider().ValueChanged([this](auto&&, auto&&)
        {
            ApplyClusterRadius();
        });

        ClusterRangeSlider().ValueChanged([this](auto&&, auto&&)
        {
            ApplyClusterRange();
        });

        ClusterToggle().Checked([this](auto&&, auto&&)
        {
            OnClusterToolToggled();
        });

        ClusterToggle().Unchecked([this](auto&&, auto&&)
        {
            OnClusterToolToggled();
        });

        DeleteClusterButton().Click([this](auto&&, auto&&)
        {
            DeleteClusterPoints();
        });

        BoundaryXMinSlider().ValueChanged([this](auto&&, auto&&) { OnBoundaryChanged(); });
        BoundaryXMaxSlider().ValueChanged([this](auto&&, auto&&) { OnBoundaryChanged(); });
        BoundaryYMinSlider().ValueChanged([this](auto&&, auto&&) { OnBoundaryChanged(); });
        BoundaryYMaxSlider().ValueChanged([this](auto&&, auto&&) { OnBoundaryChanged(); });
        BoundaryZMinSlider().ValueChanged([this](auto&&, auto&&) { OnBoundaryChanged(); });
        BoundaryZMaxSlider().ValueChanged([this](auto&&, auto&&) { OnBoundaryChanged(); });

        SaveFileButton().Click([this](auto&&, auto&&)
        {
            SaveCloudAsync(false);
        });

        SavePcdButton().Click([this](auto&&, auto&&)
        {
            SaveCloudAsync(true);
        });

        auto const host = ViewportHost();
        host.PointerPressed([this](auto&&, auto const& args) { OnPointerPressed(args); });
        host.PointerMoved([this](auto&&, auto const& args) { OnPointerMoved(args); });
        host.PointerReleased([this](auto&&, auto const& args) { OnPointerReleased(args); });
        host.PointerCaptureLost([this](auto&&, auto const& args) { OnPointerCaptureLost(args); });
        host.PointerWheelChanged([this](auto&&, auto const& args) { OnPointerWheelChanged(args); });

        // SwapChainPanel 在 Grid 上面，滚轮事件由它接收时也要处理
        auto const surface = Viewport();
        surface.PointerWheelChanged([this](auto&&, auto const& args) { OnPointerWheelChanged(args); });
        host.SizeChanged([this](auto&&, auto&&) { RequestRender(); });

        host.AllowDrop(true);
        host.DragOver([this](auto&&, auto const& args) { OnDragOver(args); });
        host.Drop([this](auto&&, auto const& args) { HandleDropAsync(args); });
    }

    void MainWindow::OpenFile()
    {
        PickFileAsync();
    }

    void MainWindow::LoadDemo()
    {
        // --demo / -d 与「示例点云 → 环面」等价；示例文件缺失时退回程序内置的示例点云
        LoadSampleCloud(L"torus.pcd", app::L(L"示例：环面", L"Sample: Torus"));
    }

    void MainWindow::LoadSampleCloud(std::wstring const& fileName, std::wstring const& displayName)
    {
        if (m_busy) return;

        std::filesystem::path const path = SampleCloudsDirectory() / fileName;
        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError))
        {
            // 只拷贝了 exe（没有带 samples\ 目录）时，至少还能看到内置示例
            SetStatus(app::L(L"未找到示例文件，已改用内置示例：", L"Sample file not found, using the built-in demo: ") + path.wstring());
            ApplyCloud(app::MakeDemoPointCloud(), app::L(L"内置示例点云", L"Built-in demo"));
            return;
        }

        LoadFileCoreAsync(path.wstring(), displayName);
    }

    void MainWindow::ResetView()
    {
        if (m_cloud.loadedVertexCount > 0)
        {
            m_camera.Frame(m_cloud.boundsMin, m_cloud.boundsMax);
        }
        RequestRender();
    }

    void MainWindow::ApplySpeedSetting()
    {
        double const speed = SpeedSlider().Value();
        m_camera.moveSpeed = static_cast<float>(speed);

        wchar_t buffer[32]{};
        std::swprintf(buffer, 32, L"%.1f×", speed);
        SpeedText().Text(buffer);
    }


    // -----------------------------------------------------------------------
    void MainWindow::ApplyClusterRadius()
    {
        m_clusterRadius = static_cast<float>(ClusterRadiusSlider().Value());
        ClusterRadiusText().Text(FormatMeters(m_clusterRadius));

        if (!m_renderer) return;

        if (m_clusterSelected && m_clusterSeedIndex < m_cloud.vertices.size())
        {
            // 参数变化后从同一个种子点重新做区域生长
            BuildClusterFromSeed(m_clusterSeedIndex);
        }
        RequestRender();
    }

    void MainWindow::ApplyClusterRange()
    {
        m_clusterRange = static_cast<float>(ClusterRangeSlider().Value());

        wchar_t buffer[32]{};
        std::swprintf(buffer, 32, L"%.1f m", m_clusterRange);
        ClusterRangeText().Text(buffer);

        if (!m_renderer) return;

        if (m_clusterSelected && m_clusterSeedIndex < m_cloud.vertices.size())
        {
            BuildClusterFromSeed(m_clusterSeedIndex);
        }
        RequestRender();
    }

    void MainWindow::OnClusterToolToggled()
    {
        if (!m_renderer) return;

        auto const checked = ClusterToggle().IsChecked();
        bool const active = checked ? checked.Value() : false;

        if (active)
        {
            m_clusterToolActive = true;
            ClusterHintText().Text(L"点击点云，自动选中与它连成一片的点云");
            SetStatus(app::L(L"点云选择：左键点击点云，会自动选中连成一片的点", L"Selection: click a point to select its connected region"));
        }
        else
        {
            m_clusterToolActive = false;
            m_clusterSelected = false;
            m_clusterIndices.clear();
            m_selectedIndices.clear();
            m_renderer->SetClusterPoints({});
            BoundaryPanel().Visibility(Visibility::Collapsed);
            DeleteClusterButton().IsEnabled(false);
            ClusterHintText().Text(L"点击点云，自动选中与它连成一片的点云");
        }
        RequestRender();
    }

    bool MainWindow::TryGetClusterSeed(float cursorX, float cursorY, std::uint32_t& seedIndex)
    {
        if (m_cloud.vertices.empty()) return false;

        float const width = static_cast<float>(Viewport().ActualWidth());
        float const height = static_cast<float>(Viewport().ActualHeight());
        if (width < 2.0f || height < 2.0f) return false;

        float const ndcX = cursorX / width * 2.0f - 1.0f;
        float const ndcY = 1.0f - cursorY / height * 2.0f;
        float const tanHalf = std::tan(m_camera.fovY * 0.5f);
        float const aspect = width / height;

        DirectX::XMFLOAT3 const eye = m_camera.Eye();
        DirectX::XMFLOAT3 const forward = m_camera.Forward();
        DirectX::XMFLOAT3 const right = m_camera.RightVector();
        DirectX::XMFLOAT3 const up = m_camera.UpVector();

        DirectX::XMVECTOR const direction = DirectX::XMVector3Normalize(DirectX::XMVectorAdd(
            DirectX::XMLoadFloat3(&forward),
            DirectX::XMVectorAdd(
                DirectX::XMVectorScale(DirectX::XMLoadFloat3(&right), ndcX * tanHalf * aspect),
                DirectX::XMVectorScale(DirectX::XMLoadFloat3(&up), ndcY * tanHalf))));

        DirectX::XMVECTOR const eyeVector = DirectX::XMLoadFloat3(&eye);
        float bestDistanceSquared = 1.0e30f;
        std::uint32_t best = 0;

        // 在点云中找离点击射线最近的点作为聚类种子
        for (std::size_t i = 0; i < m_cloud.vertices.size(); ++i)
        {
            DirectX::XMVECTOR const offset = DirectX::XMVectorSubtract(
                DirectX::XMLoadFloat3(&m_cloud.vertices[i].position), eyeVector);
            float const along = DirectX::XMVectorGetX(DirectX::XMVector3Dot(offset, direction));
            if (along <= 0.0f) continue;

            DirectX::XMVECTOR const perpendicular = DirectX::XMVectorSubtract(
                offset, DirectX::XMVectorScale(direction, along));
            float const distanceSquared = DirectX::XMVectorGetX(
                DirectX::XMVector3LengthSq(perpendicular));
            if (distanceSquared < bestDistanceSquared)
            {
                bestDistanceSquared = distanceSquared;
                best = static_cast<std::uint32_t>(i);
            }
        }

        if (bestDistanceSquared >= 1.0e30f) return false;
        seedIndex = best;
        return true;
    }

    void MainWindow::SelectClusterAt(float cursorX, float cursorY)
    {
        if (!m_renderer || m_cloud.vertices.empty()) return;

        std::uint32_t seedIndex = 0;
        if (!TryGetClusterSeed(cursorX, cursorY, seedIndex))
        {
            SetStatus(app::L(L"没有在点云上找到可选取的点", L"No selectable point found on the cloud"));
            return;
        }

        m_clusterSeedIndex = seedIndex;
        m_clusterSelected = true;
        BuildClusterFromSeed(seedIndex);
    }

    void MainWindow::BuildClusterFromSeed(std::uint32_t seedIndex)
    {
        if (!m_renderer || seedIndex >= m_cloud.vertices.size()) return;

        auto const& vertices = m_cloud.vertices;
        std::size_t const count = vertices.size();
        float const tolerance = std::max(m_clusterRadius, 0.005f);
        float const toleranceSquared = tolerance * tolerance;

        float const minX = m_cloud.boundsMin.x;
        float const minY = m_cloud.boundsMin.y;
        float const minZ = m_cloud.boundsMin.z;

        auto cellCoord = [](float value, float minValue, float size) -> std::int64_t
        {
            return static_cast<std::int64_t>(std::floor((value - minValue) / size));
        };
        auto packCell = [](std::int64_t cx, std::int64_t cy, std::int64_t cz) -> std::uint64_t
        {
            auto bits = [](std::int64_t value) -> std::uint64_t
            {
                return static_cast<std::uint64_t>(
                    std::clamp<std::int64_t>(value, 0, (1 << 21) - 1));
            };
            return (bits(cx) << 42) | (bits(cy) << 21) | bits(cz);
        };

        // 只在种子点周围“范围”滑杆指定的范围内建网格（默认 3 米，可调 0–20 米）
        float const halfExtent = std::max(m_clusterRange, 0.01f);
        DirectX::XMFLOAT3 const seed = vertices[seedIndex].position;
        float const boxMinX = seed.x - halfExtent;
        float const boxMaxX = seed.x + halfExtent;
        float const boxMinY = seed.y - halfExtent;
        float const boxMaxY = seed.y + halfExtent;
        float const boxMinZ = seed.z - halfExtent;
        float const boxMaxZ = seed.z + halfExtent;

        // 以邻接距离为单元大小建立空间网格，用于区域生长
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
        grid.reserve(1u << 16);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto const& p = vertices[i].position;
            if (p.x < boxMinX || p.x > boxMaxX ||
                p.y < boxMinY || p.y > boxMaxY ||
                p.z < boxMinZ || p.z > boxMaxZ)
            {
                continue;
            }

            grid[packCell(
                     cellCoord(p.x, minX, tolerance),
                     cellCoord(p.y, minY, tolerance),
                     cellCoord(p.z, minZ, tolerance))]
                .push_back(static_cast<std::uint32_t>(i));
        }

        constexpr std::size_t kMaxClusterPoints = 800'000;
        std::vector<std::uint8_t> visited(count, 0);
        std::vector<std::uint32_t> stack;
        stack.reserve(1024);
        stack.push_back(seedIndex);
        visited[seedIndex] = 1;

        std::vector<std::uint32_t> cluster;
        cluster.reserve(4096);

        // 区域生长：只把“距离 <= 邻接距离”的相邻点连起来，遇到空隙就断开
        while (!stack.empty() && cluster.size() < kMaxClusterPoints)
        {
            std::uint32_t const index = stack.back();
            stack.pop_back();
            cluster.push_back(index);

            auto const& p = vertices[index].position;
            std::int64_t const cx = cellCoord(p.x, minX, tolerance);
            std::int64_t const cy = cellCoord(p.y, minY, tolerance);
            std::int64_t const cz = cellCoord(p.z, minZ, tolerance);

            for (std::int64_t dx = -1; dx <= 1; ++dx)
            {
                for (std::int64_t dy = -1; dy <= 1; ++dy)
                {
                    for (std::int64_t dz = -1; dz <= 1; ++dz)
                    {
                        auto const iterator = grid.find(packCell(cx + dx, cy + dy, cz + dz));
                        if (iterator == grid.end()) continue;

                        for (std::uint32_t const neighbor : iterator->second)
                        {
                            if (visited[neighbor]) continue;

                            auto const& q = vertices[neighbor].position;
                            float const ddx = q.x - p.x;
                            float const ddy = q.y - p.y;
                            float const ddz = q.z - p.z;
                            if (ddx * ddx + ddy * ddy + ddz * ddz <= toleranceSquared)
                            {
                                visited[neighbor] = 1;
                                stack.push_back(neighbor);
                            }
                        }
                    }
                }
            }
        }

        m_clusterIndices = std::move(cluster);

        // 用聚类结果的包围盒初始化 6 个边界滑杆
        DirectX::XMFLOAT3 clusterMin{ 1.0e30f, 1.0e30f, 1.0e30f };
        DirectX::XMFLOAT3 clusterMax{ -1.0e30f, -1.0e30f, -1.0e30f };
        for (std::uint32_t const index : m_clusterIndices)
        {
            auto const& p = vertices[index].position;
            clusterMin.x = std::min(clusterMin.x, p.x);
            clusterMin.y = std::min(clusterMin.y, p.y);
            clusterMin.z = std::min(clusterMin.z, p.z);
            clusterMax.x = std::max(clusterMax.x, p.x);
            clusterMax.y = std::max(clusterMax.y, p.y);
            clusterMax.z = std::max(clusterMax.z, p.z);
        }

        if (m_clusterIndices.empty())
        {
            BoundaryPanel().Visibility(Visibility::Collapsed);
            m_selectedIndices.clear();
            m_renderer->SetClusterPoints({});
            DeleteClusterButton().IsEnabled(false);
            ClusterHintText().Text(L"没有找到可连通的点，可增大邻接距离后重试");
            SetStatus(app::L(L"点云选择：没有找到可连通的点", L"Selection: no connected points found"));
            RequestRender();
            return;
        }

        ConfigureBoundarySliders(clusterMin, clusterMax);
        ApplyBoundaryFilter();
    }

    void MainWindow::ConfigureBoundarySliders(
        DirectX::XMFLOAT3 const& min,
        DirectX::XMFLOAT3 const& max)
    {
        m_updatingBoundarySliders = true;

        auto setup = [](
            winrt::Microsoft::UI::Xaml::Controls::Slider const& slider,
            winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text,
            float low, float high, float value)
        {
            slider.Minimum(low);
            slider.Maximum(high);
            slider.StepFrequency(std::max((high - low) / 200.0f, 0.001f));
            slider.Value(value);

            // 坐标值显示到小数点后 3 位
            wchar_t buffer[32]{};
            std::swprintf(buffer, 32, L"%.3f", value);
            text.Text(buffer);
        };

        float const marginX = std::max((max.x - min.x) * 0.1f, 0.01f);
        float const marginY = std::max((max.y - min.y) * 0.1f, 0.01f);
        float const marginZ = std::max((max.z - min.z) * 0.1f, 0.01f);

        setup(BoundaryXMinSlider(), BoundaryXMinText(), min.x - marginX, max.x + marginX, min.x);
        setup(BoundaryXMaxSlider(), BoundaryXMaxText(), min.x - marginX, max.x + marginX, max.x);
        setup(BoundaryYMinSlider(), BoundaryYMinText(), min.y - marginY, max.y + marginY, min.y);
        setup(BoundaryYMaxSlider(), BoundaryYMaxText(), min.y - marginY, max.y + marginY, max.y);
        setup(BoundaryZMinSlider(), BoundaryZMinText(), min.z - marginZ, max.z + marginZ, min.z);
        setup(BoundaryZMaxSlider(), BoundaryZMaxText(), min.z - marginZ, max.z + marginZ, max.z);

        m_boundaryMin = min;
        m_boundaryMax = max;

        m_updatingBoundarySliders = false;
        BoundaryPanel().Visibility(Visibility::Visible);
    }

    void MainWindow::OnBoundaryChanged()
    {
        if (m_updatingBoundarySliders || !m_clusterSelected) return;

        m_boundaryMin = {
            static_cast<float>(BoundaryXMinSlider().Value()),
            static_cast<float>(BoundaryYMinSlider().Value()),
            static_cast<float>(BoundaryZMinSlider().Value()) };
        m_boundaryMax = {
            static_cast<float>(BoundaryXMaxSlider().Value()),
            static_cast<float>(BoundaryYMaxSlider().Value()),
            static_cast<float>(BoundaryZMaxSlider().Value()) };

        // 保证最小值不超过最大值
        if (m_boundaryMin.x > m_boundaryMax.x) std::swap(m_boundaryMin.x, m_boundaryMax.x);
        if (m_boundaryMin.y > m_boundaryMax.y) std::swap(m_boundaryMin.y, m_boundaryMax.y);
        if (m_boundaryMin.z > m_boundaryMax.z) std::swap(m_boundaryMin.z, m_boundaryMax.z);

        auto updateText = [](winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text, float value)
        {
            wchar_t buffer[32]{};
            std::swprintf(buffer, 32, L"%.3f", value);
            text.Text(buffer);
        };
        updateText(BoundaryXMinText(), m_boundaryMin.x);
        updateText(BoundaryXMaxText(), m_boundaryMax.x);
        updateText(BoundaryYMinText(), m_boundaryMin.y);
        updateText(BoundaryYMaxText(), m_boundaryMax.y);
        updateText(BoundaryZMinText(), m_boundaryMin.z);
        updateText(BoundaryZMaxText(), m_boundaryMax.z);

        ApplyBoundaryFilter();
    }

    void MainWindow::ApplyBoundaryFilter()
    {
        if (!m_renderer) return;

        std::vector<app::PlyVertex> redPoints;
        redPoints.reserve(m_clusterIndices.size());
        m_selectedIndices.clear();

        for (std::uint32_t const index : m_clusterIndices)
        {
            auto const& p = m_cloud.vertices[index].position;
            if (p.x < m_boundaryMin.x || p.x > m_boundaryMax.x ||
                p.y < m_boundaryMin.y || p.y > m_boundaryMax.y ||
                p.z < m_boundaryMin.z || p.z > m_boundaryMax.z)
            {
                continue;
            }

            m_selectedIndices.push_back(index);
            app::PlyVertex vertex = m_cloud.vertices[index];
            vertex.color = 0xFF1A1AFF;
            vertex.normal = { 0.0f, 0.0f, 0.0f };
            redPoints.push_back(vertex);
        }

        m_renderer->SetClusterPoints(redPoints);

        std::size_t const selectedCount = m_selectedIndices.size();
        DeleteClusterButton().IsEnabled(selectedCount > 0);
        ClusterHintText().Text(
            app::L(L"已选中 ", L"Selected ") + FormatCount(selectedCount) +
            app::L(L" 个点（邻接距离 ", L" points (adjacency ") +
            FormatMeters(m_clusterRadius) + app::L(L"，可调整 XYZ 边界）", L"; adjust XYZ bounds)"));
        SetStatus(
            app::L(L"点云选择：已选中 ", L"Selection: selected ") + FormatCount(selectedCount) + app::L(L" 个点", L" points"));
        RequestRender();
    }

    void MainWindow::DeleteClusterPoints()
    {
        if (!m_renderer || !m_clusterSelected || m_selectedIndices.empty() || m_cloud.vertices.empty())
        {
            return;
        }

        std::vector<std::uint8_t> selected(m_cloud.vertices.size(), 0);
        for (std::uint32_t const index : m_selectedIndices)
        {
            if (index < selected.size())
            {
                selected[index] = 1;
            }
        }

        std::size_t const before = m_cloud.vertices.size();
        std::size_t write = 0;
        for (std::size_t read = 0; read < before; ++read)
        {
            if (selected[read]) continue;
            if (write != read)
            {
                m_cloud.vertices[write] = m_cloud.vertices[read];
            }
            ++write;
        }
        m_cloud.vertices.resize(write);

        std::size_t const removed = before - write;
        if (removed == 0)
        {
            SetStatus(app::L(L"当前聚类选择内没有点", L"No points in the current selection"));
            return;
        }

        m_cloud.loadedVertexCount = m_cloud.vertices.size();
        SaveFileButton().IsEnabled(!m_cloud.vertices.empty());
        SavePcdButton().IsEnabled(!m_cloud.vertices.empty());

        // 删除后重建“所有可见点云”的合并缓冲区
        RebuildVisibleClouds();

        m_clusterSelected = false;
        m_clusterIndices.clear();
        m_selectedIndices.clear();
        m_renderer->SetClusterPoints({});
        BoundaryPanel().Visibility(Visibility::Collapsed);
        DeleteClusterButton().IsEnabled(false);

        if (m_cloud.vertices.empty())
        {
            EmptyHint().Text(app::L(L"所有点已删除", L"All points deleted"));
            EmptyHint().Visibility(Visibility::Visible);
        }

        UpdateStats();
        SetStatus(
            app::L(L"已删除 ", L"Deleted ") + FormatCount(removed) +
            app::L(L" 个点，剩余 ", L" points, remaining ") + FormatCount(m_cloud.loadedVertexCount) +
            app::L(L" 个点", L" points"));
        ClusterHintText().Text(app::L(L"点击点云，自动选中与它连成一片的点云", L"Click a point to select its connected region"));
        RequestRender();
    }


    // -----------------------------------------------------------------------
    // 指针交互
    // -----------------------------------------------------------------------
    void MainWindow::OnPointerPressed(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        // 只有点在 3D 视口上才转动/平移视角；点在悬浮面板（点云列表、视图、坐标调整）上
        // 交给面板里的控件自己处理，避免抢走按钮的点击或焦点。
        auto const source = args.OriginalSource();
        bool const overViewport = !source || source == Viewport() || source == ViewportHost();
        if (!overViewport)
        {
            return;
        }

        // 让键盘事件（W/A/S/D）能送到视口
        ViewportHost().Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);

        auto const point = args.GetCurrentPoint(ViewportHost());
        auto const properties = point.Properties();

        if (properties.IsLeftButtonPressed())
        {
            float const cursorX = static_cast<float>(point.Position().X);
            float const cursorY = static_cast<float>(point.Position().Y);

            if (m_clusterToolActive)
            {
                // 点云选择模式：先按普通左键拖动转视角；如果几乎没有移动（点击），
                // 在松开时再执行聚类选择，这样选中后仍可以用左键转视角。
                m_clusterPendingClick = true;
                m_clusterPressX = cursorX;
                m_clusterPressY = cursorY;
            }

            m_dragMode = DragMode::Look;
        }
        else if (properties.IsRightButtonPressed())
        {
            m_dragMode = DragMode::Pan;
        }
        else if (properties.IsMiddleButtonPressed())
        {
            m_dragMode = DragMode::Pan;
        }
        else
        {
            return;
        }

        m_dragPointerId = args.Pointer().PointerId();
        m_lastPointerX = point.Position().X;
        m_lastPointerY = point.Position().Y;
        ViewportHost().CapturePointer(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::OnPointerMoved(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_dragMode == DragMode::None || args.Pointer().PointerId() != m_dragPointerId) return;

        auto const point = args.GetCurrentPoint(ViewportHost());
        auto const properties = point.Properties();
        if (!properties.IsLeftButtonPressed() &&
            !properties.IsRightButtonPressed() &&
            !properties.IsMiddleButtonPressed())
        {
            m_dragMode = DragMode::None;
            ViewportHost().ReleasePointerCaptures();
            return;
        }

        float const x = point.Position().X;
        float const y = point.Position().Y;
        float const deltaX = x - m_lastPointerX;
        float const deltaY = y - m_lastPointerY;
        m_lastPointerX = x;
        m_lastPointerY = y;

        if (m_dragMode == DragMode::Look)
        {
            // 移动超过几个像素就判定为拖动转视角，不再当作点击选择
            if (m_clusterPendingClick)
            {
                float const dragX = x - m_clusterPressX;
                float const dragY = y - m_clusterPressY;
                if (dragX * dragX + dragY * dragY > 16.0f)
                {
                    m_clusterPendingClick = false;
                }
            }
            m_camera.Look(-deltaX, -deltaY);
        }
        else
        {
            m_camera.Pan(deltaX, deltaY, static_cast<float>(Viewport().ActualHeight()));
        }

        RequestRender();
        args.Handled(true);
    }

    void MainWindow::OnPointerReleased(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (args.Pointer().PointerId() != m_dragPointerId) return;

        bool const wasClick = m_clusterPendingClick;
        m_clusterPendingClick = false;
        m_dragMode = DragMode::None;
        ViewportHost().ReleasePointerCaptures();

        if (wasClick && m_clusterToolActive)
        {
            SelectClusterAt(m_clusterPressX, m_clusterPressY);
        }

        args.Handled(true);
    }

    void MainWindow::OnPointerCaptureLost(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (args.Pointer().PointerId() == m_dragPointerId)
        {
            m_dragMode = DragMode::None;
            m_clusterPendingClick = false;
        }
    }

    void MainWindow::OnPointerWheelChanged(winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        int const delta = args.GetCurrentPoint(ViewportHost()).Properties().MouseWheelDelta();
        if (delta == 0) return;

        m_camera.Move(static_cast<float>(delta));
        RequestRender();
        args.Handled(true);
    }

    // -----------------------------------------------------------------------
    // 顶部菜单
    // -----------------------------------------------------------------------
    void MainWindow::OnMenuOpenClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        OpenFile();
    }

    void MainWindow::OnMenuSaveClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        SaveCloudAsync(false);
    }

    void MainWindow::OnMenuSavePcdClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        SaveCloudAsync(true);
    }

    void MainWindow::OnMenuExitClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        Close();
    }

    void MainWindow::OnMenuSelectModeClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto const checked = ClusterToggle().IsChecked();
        ClusterToggle().IsChecked(!(checked && checked.Value()));
    }

    void MainWindow::OnMenuDeletePointsClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        DeleteClusterPoints();
    }

    void MainWindow::OnMenuClearSelectionClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ClusterToggle().IsChecked(false);
    }

    void MainWindow::OnMenuResetViewClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ResetView();
    }

    winrt::fire_and_forget MainWindow::ShowAboutAsync()
    {
        winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog;
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.Title(winrt::box_value(app::L(L"关于 3D点云编辑器", L"About 3D Point Cloud Editor")));
        dialog.CloseButtonText(app::L(L"确定", L"OK"));
        co_await dialog.ShowAsync();
    }

    void MainWindow::OnMenuAboutClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ShowAboutAsync();
    }

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // 多点点云列表（勾选 = 同时显示）
    // -----------------------------------------------------------------------
    void MainWindow::UpdateCloudList()
    {
        m_updatingCloudList = true;

        auto const panel = CloudListPanel();
        panel.Children().Clear();

        auto const accentBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::Color{ 0xFF, 0x2B, 0x4C, 0x7E });
        auto const whiteBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::Color{ 0xFF, 0xFF, 0xFF, 0xFF });

        auto addRow = [&](std::uint64_t id, std::wstring const& name, std::size_t count, bool visible, bool active)
        {
            winrt::Microsoft::UI::Xaml::Controls::CheckBox check;
            check.IsChecked(visible);
            check.MinWidth(0);
            check.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

            winrt::Microsoft::UI::Xaml::Controls::Button nameButton;
            std::wstring const label = name + L"  (" + FormatCount(count) + app::L(L" 点)", L" pts)");
            nameButton.Content(winrt::box_value(winrt::hstring(label)));
            nameButton.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            nameButton.HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left);
            if (active)
            {
                // 当前点云用颜色突出显示（不改变列表顺序）
                nameButton.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
                nameButton.Background(accentBrush);
                nameButton.Foreground(whiteBrush);
            }

            nameButton.Click([this, id](auto&&, auto&&) { ActivateCloudById(id); });
            check.Checked([this, id](auto&&, auto&&) { SetCloudVisible(id, true); });
            check.Unchecked([this, id](auto&&, auto&&) { SetCloudVisible(id, false); });

            winrt::Microsoft::UI::Xaml::Controls::StackPanel row;
            row.Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
            row.Spacing(6);
            row.Children().Append(check);
            row.Children().Append(nameButton);
            panel.Children().Append(row);
        };

        // 按加载顺序显示；当前点云保持原位置，只做颜色高亮
        for (std::uint64_t const id : m_cloudOrder)
        {
            if (id == m_activeCloudId)
            {
                addRow(id, m_displayName, m_cloud.vertices.size(), m_activeCloudVisible, true);
                continue;
            }

            auto const iterator = std::find_if(
                m_otherClouds.begin(),
                m_otherClouds.end(),
                [id](StoredCloud const& stored) { return stored.id == id; });
            if (iterator == m_otherClouds.end()) continue;

            addRow(iterator->id, iterator->name, iterator->cloud.vertices.size(), iterator->visible, false);
        }

        m_updatingCloudList = false;
    }

    void MainWindow::RebuildVisibleClouds()
    {
        if (!m_renderer) return;

        std::size_t total = 0;
        if (m_activeCloudVisible) total += m_cloud.vertices.size();
        for (auto const& stored : m_otherClouds)
        {
            if (stored.visible) total += stored.cloud.vertices.size();
        }

        std::vector<app::PlyVertex> combined;
        combined.reserve(total);
        if (m_activeCloudVisible)
        {
            combined.insert(combined.end(), m_cloud.vertices.begin(), m_cloud.vertices.end());
        }
        for (auto const& stored : m_otherClouds)
        {
            if (stored.visible)
            {
                combined.insert(combined.end(), stored.cloud.vertices.begin(), stored.cloud.vertices.end());
            }
        }

        try
        {
            m_renderer->SetPoints(combined);
        }
        catch (std::exception const& error)
        {
            ShowError(L"上传点云数据到 GPU 失败：" + Utf8ToWide(error.what()));
        }
    }

    void MainWindow::SetCloudVisible(std::uint64_t id, bool visible)
    {
        if (m_updatingCloudList) return;

        if (id == m_activeCloudId)
        {
            m_activeCloudVisible = visible;
        }
        else
        {
            for (auto& stored : m_otherClouds)
            {
                if (stored.id == id)
                {
                    stored.visible = visible;
                    break;
                }
            }
        }

        RebuildVisibleClouds();
        UpdateStats();
        SetStatus(visible ? app::L(L"已显示点云", L"Cloud shown") : app::L(L"已隐藏点云", L"Cloud hidden"));
        RequestRender();
    }

    void MainWindow::ActivateCloudById(std::uint64_t id)
    {
        if (!m_renderer || id == m_activeCloudId) return;

        auto const target = std::find_if(
            m_otherClouds.begin(),
            m_otherClouds.end(),
            [id](StoredCloud const& stored) { return stored.id == id; });
        if (target == m_otherClouds.end()) return;

        std::size_t const index = static_cast<std::size_t>(target - m_otherClouds.begin());

        if (!m_cloud.vertices.empty() || !m_displayName.empty())
        {
            m_otherClouds.push_back({ m_activeCloudId, m_displayName, std::move(m_cloud), m_activeCloudVisible, m_activeTransform });
        }
        m_cloud = app::PlyCloud{};

        m_cloud = std::move(m_otherClouds[index].cloud);
        m_displayName = m_otherClouds[index].name;
        m_activeCloudId = m_otherClouds[index].id;
        m_activeCloudVisible = m_otherClouds[index].visible;
        m_activeTransform = m_otherClouds[index].transform;
        m_otherClouds.erase(m_otherClouds.begin() + static_cast<std::ptrdiff_t>(index));

        RebuildVisibleClouds();

        m_camera.Frame(m_cloud.boundsMin, m_cloud.boundsMax);

        EmptyHint().Visibility(Visibility::Collapsed);
        ClusterToggle().IsEnabled(true);
        SaveFileButton().IsEnabled(!m_cloud.vertices.empty());
        SavePcdButton().IsEnabled(!m_cloud.vertices.empty());
        DeleteClusterButton().IsEnabled(false);
        BoundaryPanel().Visibility(Visibility::Collapsed);
        m_clusterSelected = false;
        m_clusterIndices.clear();
        m_selectedIndices.clear();
        m_renderer->SetClusterPoints({});

        UpdateCloudList();
        UpdateStats();
        UpdateTransformLabels();
        SetStatus(
            app::L(L"当前点云：", L"Active cloud: ") + m_displayName + L" · " +
            FormatCount(m_cloud.vertices.size()) + app::L(L" 个点", L" points"));
        RequestRender();
    }


    // -----------------------------------------------------------------------
    // 视图预设：俯/仰、前/后、左/右（世界坐标 Z 轴向上）
    // -----------------------------------------------------------------------
    void MainWindow::ApplyViewPreset(float yaw, float pitch)
    {
        m_camera.yaw = yaw;
        m_camera.pitch = std::clamp(
            pitch,
            -app::OrbitCamera::kPitchLimit,
            app::OrbitCamera::kPitchLimit);
        RequestRender();
    }

    void MainWindow::OnViewTopClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // 从上方往下看
        ApplyViewPreset(m_camera.yaw, 1.5533f);
    }

    void MainWindow::OnViewBottomClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // 从下方往上看
        ApplyViewPreset(m_camera.yaw, -1.5533f);
    }

    void MainWindow::OnViewFrontClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // 前视：X 朝右、Z 朝上
        ApplyViewPreset(-DirectX::XM_PIDIV2, 0.0f);
    }

    void MainWindow::OnViewBackClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyViewPreset(DirectX::XM_PIDIV2, 0.0f);
    }

    void MainWindow::OnViewLeftClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyViewPreset(DirectX::XM_PI, 0.0f);
    }

    void MainWindow::OnViewRightClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyViewPreset(0.0f, 0.0f);
    }

    void MainWindow::OnAxesToggleClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto const checked = AxesToggle().IsChecked();
        m_axesVisible = checked && checked.Value();
        RequestRender();
    }

    // -----------------------------------------------------------------------
    // 点云坐标调整（平移 / 旋转当前点云）
    // -----------------------------------------------------------------------
    float MainWindow::TranslationStep()
    {
        std::wstring const text(TranslationStepBox().Text().c_str());
        wchar_t* end = nullptr;
        float const value = std::wcstof(text.c_str(), &end);
        if (end != text.c_str() && std::isfinite(value) && value > 0.0f)
        {
            return value;
        }
        return 0.1f;
    }

    float MainWindow::RotationStep()
    {
        std::wstring const text(RotationStepBox().Text().c_str());
        wchar_t* end = nullptr;
        float const value = std::wcstof(text.c_str(), &end);
        if (end != text.c_str() && std::isfinite(value) && value > 0.0f)
        {
            return value;
        }
        return 5.0f;
    }

    void MainWindow::UpdateTransformLabels()
    {
        wchar_t buffer[48]{};

        std::swprintf(buffer, 48, L"%.3f", m_activeTransform.offsetX);
        TransformXText().Text(buffer);
        std::swprintf(buffer, 48, L"%.3f", m_activeTransform.offsetY);
        TransformYText().Text(buffer);
        std::swprintf(buffer, 48, L"%.3f", m_activeTransform.offsetZ);
        TransformZText().Text(buffer);

        std::swprintf(buffer, 48, L"%.3f°", m_activeTransform.rotX);
        TransformRXText().Text(buffer);
        std::swprintf(buffer, 48, L"%.3f°", m_activeTransform.rotY);
        TransformRYText().Text(buffer);
        std::swprintf(buffer, 48, L"%.3f°", m_activeTransform.rotZ);
        TransformRZText().Text(buffer);
    }

    void MainWindow::ApplyTranslation(float dx, float dy, float dz)
    {
        if (!m_renderer || m_cloud.vertices.empty()) return;

        for (auto& vertex : m_cloud.vertices)
        {
            vertex.position.x += dx;
            vertex.position.y += dy;
            vertex.position.z += dz;
        }

        m_cloud.boundsMin.x += dx;
        m_cloud.boundsMin.y += dy;
        m_cloud.boundsMin.z += dz;
        m_cloud.boundsMax.x += dx;
        m_cloud.boundsMax.y += dy;
        m_cloud.boundsMax.z += dz;

        m_activeTransform.offsetX += dx;
        m_activeTransform.offsetY += dy;
        m_activeTransform.offsetZ += dz;

        RebuildVisibleClouds();
        UpdateTransformLabels();
        SetStatus(app::L(L"当前点云已平移", L"Current cloud translated"));
        RequestRender();
    }

    void MainWindow::ApplyRotation(float rx, float ry, float rz)
    {
        if (!m_renderer || m_cloud.vertices.empty()) return;

        DirectX::XMFLOAT3 const center{
            (m_cloud.boundsMin.x + m_cloud.boundsMax.x) * 0.5f,
            (m_cloud.boundsMin.y + m_cloud.boundsMax.y) * 0.5f,
            (m_cloud.boundsMin.z + m_cloud.boundsMax.z) * 0.5f };

        DirectX::XMMATRIX const rotation =
            DirectX::XMMatrixRotationX(DirectX::XMConvertToRadians(rx)) *
            DirectX::XMMatrixRotationY(DirectX::XMConvertToRadians(ry)) *
            DirectX::XMMatrixRotationZ(DirectX::XMConvertToRadians(rz));

        DirectX::XMVECTOR const centerVector = DirectX::XMLoadFloat3(&center);
        for (auto& vertex : m_cloud.vertices)
        {
            DirectX::XMVECTOR point = DirectX::XMVectorSubtract(
                DirectX::XMLoadFloat3(&vertex.position), centerVector);
            point = DirectX::XMVector3Transform(point, rotation);
            DirectX::XMStoreFloat3(
                &vertex.position, DirectX::XMVectorAdd(point, centerVector));
        }

        // 重新计算包围盒
        m_cloud.boundsMin = { 1.0e30f, 1.0e30f, 1.0e30f };
        m_cloud.boundsMax = { -1.0e30f, -1.0e30f, -1.0e30f };
        for (auto const& vertex : m_cloud.vertices)
        {
            m_cloud.boundsMin.x = std::min(m_cloud.boundsMin.x, vertex.position.x);
            m_cloud.boundsMin.y = std::min(m_cloud.boundsMin.y, vertex.position.y);
            m_cloud.boundsMin.z = std::min(m_cloud.boundsMin.z, vertex.position.z);
            m_cloud.boundsMax.x = std::max(m_cloud.boundsMax.x, vertex.position.x);
            m_cloud.boundsMax.y = std::max(m_cloud.boundsMax.y, vertex.position.y);
            m_cloud.boundsMax.z = std::max(m_cloud.boundsMax.z, vertex.position.z);
        }

        m_activeTransform.rotX += rx;
        m_activeTransform.rotY += ry;
        m_activeTransform.rotZ += rz;

        RebuildVisibleClouds();
        UpdateTransformLabels();
        SetStatus(app::L(L"当前点云已旋转", L"Current cloud rotated"));
        RequestRender();
    }

    void MainWindow::OnTransformXMinusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyTranslation(-TranslationStep(), 0.0f, 0.0f);
    }

    void MainWindow::OnTransformXPlusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyTranslation(TranslationStep(), 0.0f, 0.0f);
    }

    void MainWindow::OnTransformYMinusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyTranslation(0.0f, -TranslationStep(), 0.0f);
    }

    void MainWindow::OnTransformYPlusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyTranslation(0.0f, TranslationStep(), 0.0f);
    }

    void MainWindow::OnTransformZMinusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyTranslation(0.0f, 0.0f, -TranslationStep());
    }

    void MainWindow::OnTransformZPlusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyTranslation(0.0f, 0.0f, TranslationStep());
    }

    void MainWindow::OnTransformRXMinusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyRotation(-RotationStep(), 0.0f, 0.0f);
    }

    void MainWindow::OnTransformRXPlusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyRotation(RotationStep(), 0.0f, 0.0f);
    }

    void MainWindow::OnTransformRYMinusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyRotation(0.0f, -RotationStep(), 0.0f);
    }

    void MainWindow::OnTransformRYPlusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyRotation(0.0f, RotationStep(), 0.0f);
    }

    void MainWindow::OnTransformRZMinusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyRotation(0.0f, 0.0f, -RotationStep());
    }

    void MainWindow::OnTransformRZPlusClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        ApplyRotation(0.0f, 0.0f, RotationStep());
    }

    // -----------------------------------------------------------------------
    // 键盘移动：按住 W/S 前进/后退，A/D 左移/右移（可与左键拖动同时进行）
    // -----------------------------------------------------------------------
    void MainWindow::OnViewportKeyDown(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
    {
        switch (args.Key())
        {
        case winrt::Windows::System::VirtualKey::W: m_keyW = true; break;
        case winrt::Windows::System::VirtualKey::S: m_keyS = true; break;
        case winrt::Windows::System::VirtualKey::A: m_keyA = true; break;
        case winrt::Windows::System::VirtualKey::D: m_keyD = true; break;
        case winrt::Windows::System::VirtualKey::Q: m_keyQ = true; break;
        case winrt::Windows::System::VirtualKey::E: m_keyE = true; break;
        default: return;
        }

        args.Handled(true);
        if (m_moveTimer)
        {
            m_moveTimer.Start();
        }
    }

    void MainWindow::OnViewportKeyUp(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
    {
        switch (args.Key())
        {
        case winrt::Windows::System::VirtualKey::W: m_keyW = false; break;
        case winrt::Windows::System::VirtualKey::S: m_keyS = false; break;
        case winrt::Windows::System::VirtualKey::A: m_keyA = false; break;
        case winrt::Windows::System::VirtualKey::D: m_keyD = false; break;
        case winrt::Windows::System::VirtualKey::Q: m_keyQ = false; break;
        case winrt::Windows::System::VirtualKey::E: m_keyE = false; break;
        default: return;
        }

        args.Handled(true);
        if (!m_keyW && !m_keyS && !m_keyA && !m_keyD && !m_keyQ && !m_keyE)
        {
            StopKeyboardMove();
        }
    }

    void MainWindow::OnViewportLostFocus(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        StopKeyboardMove();
    }

    void MainWindow::StopKeyboardMove()
    {
        m_keyW = false;
        m_keyS = false;
        m_keyA = false;
        m_keyD = false;
        m_keyQ = false;
        m_keyE = false;
        if (m_moveTimer)
        {
            m_moveTimer.Stop();
        }
    }

    void MainWindow::OnMoveTick()
    {
        if (!m_renderer || (!m_keyW && !m_keyS && !m_keyA && !m_keyD && !m_keyQ && !m_keyE))
        {
            StopKeyboardMove();
            return;
        }

        constexpr float kFrameSeconds = 0.016f;
        float const speed = std::clamp(m_camera.moveSpeed, 0.05f, 10.0f);
        float const amount = m_camera.distance * 0.6f * speed * kFrameSeconds;

        if (m_keyW) m_camera.MoveForward(amount);
        if (m_keyS) m_camera.MoveForward(-amount);
        if (m_keyD) m_camera.Strafe(amount);
        if (m_keyA) m_camera.Strafe(-amount);
        if (m_keyQ) m_camera.MoveUp(amount);
        if (m_keyE) m_camera.MoveUp(-amount);

        RequestRender();
    }

    void MainWindow::OnDragOver(DragEventArgs const& args)
    {
        if (args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
        {
            args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
        }
    }

    // -----------------------------------------------------------------------
    // 文件加载
    // -----------------------------------------------------------------------
    winrt::fire_and_forget MainWindow::PickFileAsync()
    {
        if (m_busy) co_return;

        std::optional<std::wstring> path;
        try
        {
            path = ShowOpenFileDialog(app::MainWindowHandle);
        }
        catch (winrt::hresult_error const& error)
        {
            ShowError(L"打开文件选择器失败：" + std::wstring(error.message().c_str()));
            co_return;
        }

        if (!path) co_return;
        co_await LoadFileCoreAsync(*path);
    }

    // 保存当前点云（含删除球编辑后的结果）为 binary_little_endian PLY。
    winrt::fire_and_forget MainWindow::SaveCloudAsync(bool pcd)
    {
        if (m_busy || !m_renderer) co_return;
        if (m_cloud.vertices.empty())
        {
            ShowError(L"没有可保存的点云数据。");
            co_return;
        }

        std::wstring suggested = m_displayName;
        std::wstring lower = suggested;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (lower.size() > 4 && (lower.rfind(L".ply") == lower.size() - 4 || lower.rfind(L".pcd") == lower.size() - 4))
        {
            suggested.resize(suggested.size() - 4);
        }
        if (suggested.empty()) suggested = L"pointcloud";

        std::optional<std::wstring> path;
        try
        {
            path = ShowSaveFileDialog(app::MainWindowHandle, suggested, pcd);
        }
        catch (winrt::hresult_error const& error)
        {
            ShowError(L"打开保存对话框失败：" + std::wstring(error.message().c_str()));
            co_return;
        }

        if (!path) co_return;

        SetBusy(true);
        SetStatus(app::L(L"正在保存：", L"Saving: ") + *path);

        // 直接在 UI 线程写文件：写入期间不允许编辑，避免和后台线程争用顶点数据。
        try
        {
            if (pcd)
            {
                app::SavePcdFile(*path, m_cloud.vertices);
            }
            else
            {
                app::SavePlyFile(*path, m_cloud.vertices);
            }
            SetBusy(false);
            SetStatus(app::L(L"已保存 ", L"Saved ") + FormatCount(m_cloud.vertices.size()) + app::L(L" 个点到 ", L" points to ") + *path);
        }
        catch (std::exception const& error)
        {
            SetBusy(false);
            ShowError(
                L"保存失败：" + Utf8ToWide(error.what()) + L"\n文件：" + *path +
                L"\n请换一个可写的文件夹（例如“文档”或桌面）后重试。");
        }
    }

    winrt::fire_and_forget MainWindow::HandleDropAsync(winrt::Microsoft::UI::Xaml::DragEventArgs args)
    {
        auto const deferral = args.GetDeferral();
        auto const dataView = args.DataView();

        std::wstring path;
        try
        {
            auto const items = co_await dataView.GetStorageItemsAsync();
            for (auto const& item : items)
            {
                auto const file = item.try_as<winrt::Windows::Storage::StorageFile>();
                if (!file) continue;

                std::wstring const candidate(file.Path().c_str());
                if (EndsWith(ToLower(candidate), L".ply") || EndsWith(ToLower(candidate), L".pcd"))
                {
                    path = candidate;
                    break;
                }
            }
        }
        catch (...)
        {
        }
        deferral.Complete();

        if (!path.empty()) co_await LoadFileCoreAsync(path);
    }

    winrt::fire_and_forget MainWindow::LoadStartupFileAsync(std::wstring path)
    {
        co_await LoadFileCoreAsync(std::move(path));
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::LoadFileCoreAsync(std::wstring path)
    {
        // 不指定显示名时用文件名
        co_await LoadFileCoreAsync(std::move(path), std::wstring{});
        co_return;
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::LoadFileCoreAsync(std::wstring path, std::wstring displayName)
    {
        if (m_busy) co_return;

        SetBusy(true);
        SetStatus(app::L(L"正在加载：", L"Loading: ") + path);

        auto const dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        std::wstring errorMessage;
        std::optional<app::PlyCloud> cloud;

        co_await winrt::resume_background();
        try
        {
            cloud = app::LoadPointCloudFile(path);
        }
        catch (std::bad_alloc const&)
        {
            errorMessage = L"内存不足：点云数据过大，无法加载。";
        }
        catch (std::exception const& error)
        {
            errorMessage = Utf8ToWide(error.what());
        }
        catch (...)
        {
            errorMessage = L"未知错误。";
        }

        if (!dispatcher)
        {
            co_return;
        }
        co_await ResumeOnDispatcher(dispatcher);

        SetBusy(false);
        if (!cloud)
        {
            ShowError(L"加载失败：" + errorMessage);
            co_return;
        }

        std::filesystem::path const filePath(path);
        ApplyCloud(std::move(*cloud), displayName.empty() ? filePath.filename().wstring() : displayName);
    }

    void MainWindow::ApplyCloud(app::PlyCloud cloud, std::wstring displayName)
    {
        if (!m_renderer)
        {
            ShowError(L"渲染器尚未初始化，无法显示点云。");
            return;
        }

        // 已经有一个点云时，先把它存进列表，支持同时打开多个点云
        if (!m_cloud.vertices.empty() || !m_displayName.empty())
        {
            m_otherClouds.push_back({ m_activeCloudId, m_displayName, std::move(m_cloud), m_activeCloudVisible, m_activeTransform });
        }

        m_cloud = std::move(cloud);
        m_displayName = std::move(displayName);
        m_activeCloudId = m_nextCloudId++;
        m_activeCloudVisible = true;
        m_activeTransform = CloudTransform{};
        m_cloudOrder.push_back(m_activeCloudId);

        RebuildVisibleClouds();

        // 保留 CPU 端顶点副本：删除球功能需要按位置过滤点（内存占用与 GPU 顶点缓冲相同）。
        // 如果后续需要支持超大点云，可以改成按需从文件重读。

        m_camera.Frame(m_cloud.boundsMin, m_cloud.boundsMax);

        EmptyHint().Visibility(Visibility::Collapsed);
        ClusterToggle().IsEnabled(true);
        SaveFileButton().IsEnabled(true);
        SavePcdButton().IsEnabled(true);
        DeleteClusterButton().IsEnabled(false);
        BoundaryPanel().Visibility(Visibility::Collapsed);
        m_clusterSelected = false;
        m_clusterIndices.clear();
        m_selectedIndices.clear();
        if (m_renderer)
        {
            m_renderer->SetClusterPoints({});
        }
        UpdateStats();

        std::wstring status = app::L(L"已加载 ", L"Loaded ") + m_displayName + L" · " +
            FormatCount(m_cloud.loadedVertexCount) + app::L(L" 个点", L" points");
        if (m_cloud.format == "generated")
        {
            status += app::L(L" · 内置示例", L" · built-in demo");
        }
        else
        {
            status += L" · " + Utf8ToWide(m_cloud.format) + L" · " + FormatBytes(m_cloud.fileSizeBytes);
        }
        status += app::L(L" · Z 轴向上", L" · Z-up");
        SetStatus(status);

        UpdateCloudList();
        UpdateTransformLabels();
        RequestRender();
    }

    // -----------------------------------------------------------------------
    // 渲染
    // -----------------------------------------------------------------------
    void MainWindow::RequestRender()
    {
        if (!m_renderer) return;
        if (m_renderTimer && !m_renderTimer.IsRunning())
        {
            m_renderTimer.Start();
        }
    }

    // 根据当前语言刷新所有界面文字（中文 / English）。
    void MainWindow::ApplyLanguage()
    {
        // ---- 菜单 ----
        MenuFile().Title(app::L(L"文件", L"File"));
        MenuOpenItem().Text(app::L(L"打开点云文件…", L"Open Point Cloud…"));
        MenuSavePlyItem().Text(app::L(L"保存 PLY 文件…", L"Save PLY File…"));
        MenuSavePcdItem().Text(app::L(L"保存 PCD 文件…", L"Save PCD File…"));
        MenuExitItem().Text(app::L(L"退出", L"Exit"));
        MenuEdit().Title(app::L(L"编辑", L"Edit"));
        MenuSelectModeItem().Text(app::L(L"点云选择模式", L"Point Selection Mode"));
        MenuDeleteItem().Text(app::L(L"删除选中的点云", L"Delete Selected Points"));
        MenuClearSelectItem().Text(app::L(L"清除选择", L"Clear Selection"));
        MenuView().Title(app::L(L"视图", L"View"));
        MenuResetViewItem().Text(app::L(L"重置视角", L"Reset View"));
        MenuLanguage().Title(app::L(L"语言", L"Language"));
        MenuTheme().Title(app::L(L"主题", L"Theme"));
        ThemeSystemItem().Text(app::L(L"跟随系统", L"Follow System"));
        ThemeLightItem().Text(app::L(L"浅色", L"Light"));
        ThemeDarkItem().Text(app::L(L"深色", L"Dark"));
        MenuHelp().Title(app::L(L"帮助", L"Help"));
        MenuAboutItem().Text(app::L(L"关于", L"About"));

        // ---- 工具栏 ----
        OpenButton().Content(winrt::box_value(app::L(L"打开点云文件…", L"Open Point Cloud…")));
        DemoButton().Content(winrt::box_value(app::L(L"示例点云", L"Demo Cloud")));
        DemoSphereItem().Text(app::L(L"球体（PLY ASCII）", L"Sphere (PLY ASCII)"));
        DemoWaveItem().Text(app::L(L"波浪曲面（PLY 二进制小端）", L"Wave (PLY binary LE)"));
        DemoHelixItem().Text(app::L(L"螺旋线（PLY 二进制大端）", L"Helix (PLY binary BE)"));
        DemoTorusItem().Text(app::L(L"环面（PCD）", L"Torus (PCD)"));
        SaveFileButton().Content(winrt::box_value(app::L(L"保存 PLY…", L"Save PLY…")));
        SavePcdButton().Content(winrt::box_value(app::L(L"保存 PCD…", L"Save PCD…")));
        ResetButton().Content(winrt::box_value(app::L(L"重置视角", L"Reset View")));
        PointSizeLabel().Text(app::L(L"点大小", L"Point Size"));
        SpeedLabel().Text(app::L(L"视角速度", L"View Speed"));

        ClusterToggle().Content(winrt::box_value(app::L(L"选择点云", L"Select Points")));
        AdjacencyLabel().Text(app::L(L"邻接距离", L"Adjacency"));
        RangeLabel().Text(app::L(L"范围", L"Range"));
        DeleteClusterButton().Content(winrt::box_value(app::L(L"删除点云", L"Delete Points")));
        ClusterHintText().Text(app::L(
            L"点击点云选中连通的一片；左键拖动转视角",
            L"Click a point to select its connected region; drag to rotate"));

        BoundaryXMinLabel().Text(app::L(L"最小", L"Min"));
        BoundaryXMaxLabel().Text(app::L(L"最大", L"Max"));
        BoundaryYMinLabel().Text(app::L(L"最小", L"Min"));
        BoundaryYMaxLabel().Text(app::L(L"最大", L"Max"));
        BoundaryZMinLabel().Text(app::L(L"最小", L"Min"));
        BoundaryZMaxLabel().Text(app::L(L"最大", L"Max"));

        // ---- 左侧点云列表 ----
        CloudListTitle().Text(app::L(L"点云列表", L"Point Clouds"));
        CloudListHint().Text(app::L(
            L"勾选可同时显示多个点云；点击名称切换当前编辑的点云",
            L"Check to show multiple clouds; click a name to make it active"));
        EmptyHint().Text(app::L(
            L"将 .ply / .pcd 文件拖入窗口，或点击“打开点云文件…”",
            L"Drop a .ply / .pcd file here, or click “Open Point Cloud…”"));

        // ---- 左下操作提示 ----
        ControlsHint().Text(app::L(
            L"左键拖动：按拖动方向转动视角    右键 / 中键拖动：平移    滚轮：前进 / 后退    W / S：前进 / 后退    A / D：左移 / 右移    Q / E：上升 / 下降",
            L"Left-drag: rotate     Right/Middle-drag: pan     Wheel: forward / back    W / S: forward / back    A / D: strafe    Q / E: up / down"));

        // ---- 右侧面板 ----
        ViewPanelTitle().Text(app::L(L"视图", L"View"));
        ViewTopButton().Content(winrt::box_value(app::L(L"俯视", L"Top")));
        ViewBottomButton().Content(winrt::box_value(app::L(L"仰视", L"Bottom")));
        ViewFrontButton().Content(winrt::box_value(app::L(L"前视", L"Front")));
        ViewBackButton().Content(winrt::box_value(app::L(L"后视", L"Back")));
        ViewLeftButton().Content(winrt::box_value(app::L(L"左视", L"Left")));
        ViewRightButton().Content(winrt::box_value(app::L(L"右视", L"Right")));
        AxesToggle().Content(winrt::box_value(app::L(L"显示坐标轴", L"Show Axes")));

        TransformPanelTitle().Text(app::L(L"坐标调整", L"Transform"));
        StepLabel().Text(app::L(L"步长", L"Step"));
        AngleLabel().Text(app::L(L"角度", L"Angle"));

        // ---- 语言菜单勾选状态 ----
        bool const english = app::IsEnglish();
        LangChineseItem().IsChecked(!english);
        LangEnglishItem().IsChecked(english);

        // ---- 窗口标题 ----
        try
        {
            Title(app::L(L"3D点云编辑器", L"3D Point Cloud Editor"));
        }
        catch (...)
        {
        }
    }

    void MainWindow::OnLanguageChineseClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        app::SetLanguage(app::Language::Chinese);
        ApplyLanguage();
        SetStatus(L"界面语言已切换为中文");
        RequestRender();
    }

    void MainWindow::OnLanguageEnglishClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        app::SetLanguage(app::Language::English);
        ApplyLanguage();
        SetStatus(L"Language switched to English");
        RequestRender();
    }

    // 根据“主题”菜单的选择（跟随系统 / 浅色 / 深色）切换界面配色；
    // 选择“跟随系统”时，系统主题变化会由 UISettings 事件再次调用。
    void MainWindow::ApplyTheme()
    {
        bool light = true;
        if (m_themePreference == ThemePreference::Light)
        {
            light = true;
        }
        else if (m_themePreference == ThemePreference::Dark)
        {
            light = false;
        }
        else if (m_uiSettings)
        {
            try
            {
                auto const background = m_uiSettings.GetColorValue(
                    winrt::Windows::UI::ViewManagement::UIColorType::Background);
                light = (static_cast<int>(background.R) + static_cast<int>(background.G) +
                         static_cast<int>(background.B)) > 3 * 128;
            }
            catch (...)
            {
            }
        }

        RootGrid().RequestedTheme(
            light ? winrt::Microsoft::UI::Xaml::ElementTheme::Light
                  : winrt::Microsoft::UI::Xaml::ElementTheme::Dark);

        if (m_renderer)
        {
            if (light)
            {
                m_renderer->SetClearColor(0.945f, 0.949f, 0.961f);
            }
            else
            {
                m_renderer->SetClearColor(0.055f, 0.063f, 0.078f);
            }
        }

        // 主题菜单勾选状态
        ThemeSystemItem().IsChecked(m_themePreference == ThemePreference::System);
        ThemeLightItem().IsChecked(m_themePreference == ThemePreference::Light);
        ThemeDarkItem().IsChecked(m_themePreference == ThemePreference::Dark);

        RequestRender();
    }

    void MainWindow::OnThemeSystemClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_themePreference = ThemePreference::System;
        ApplyTheme();
        SetStatus(app::L(L"主题：跟随系统", L"Theme: follow system"));
        RequestRender();
    }

    void MainWindow::OnThemeLightClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_themePreference = ThemePreference::Light;
        ApplyTheme();
        SetStatus(app::L(L"主题：浅色", L"Theme: light"));
        RequestRender();
    }

    void MainWindow::OnThemeDarkClick(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        m_themePreference = ThemePreference::Dark;
        ApplyTheme();
        SetStatus(app::L(L"主题：深色", L"Theme: dark"));
        RequestRender();
    }

    void MainWindow::OnRenderTick()
    {
        RenderFrame();
    }

    // 把世界坐标轴的方向投影到右下角的 2D 指示器上（X 红、Y 绿、Z 蓝）。
    void MainWindow::UpdateAxisGizmo(DirectX::XMMATRIX const& view)
    {
        DirectX::XMFLOAT3 const axes[3] = {
            { 1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f } };

        Microsoft::UI::Xaml::Shapes::Line lines[3] = { AxisLineX(), AxisLineY(), AxisLineZ() };
        Microsoft::UI::Xaml::Controls::TextBlock labels[3] = { AxisLabelX(), AxisLabelY(), AxisLabelZ() };

        constexpr float center = 48.0f;
        constexpr float axisLength = 32.0f;

        for (int index = 0; index < 3; ++index)
        {
            DirectX::XMVECTOR const direction = DirectX::XMLoadFloat3(&axes[index]);
            DirectX::XMFLOAT3 viewDirection{};
            DirectX::XMStoreFloat3(&viewDirection, DirectX::XMVector3TransformNormal(direction, view));

            float const endX = center + viewDirection.x * axisLength;
            float const endY = center - viewDirection.y * axisLength;

            lines[index].X1(center);
            lines[index].Y1(center);
            lines[index].X2(endX);
            lines[index].Y2(endY);

            Microsoft::UI::Xaml::Controls::Canvas::SetLeft(labels[index], endX + 2.0f);
            Microsoft::UI::Xaml::Controls::Canvas::SetTop(labels[index], endY - 7.0f);
        }
    }

    void MainWindow::RenderFrame()
    {
        if (!m_renderer) return;

        try
        {
            // 布局尚未完成时不要用 1x1 的交换链去绘制；稍后重试。
            if (Viewport().ActualWidth() < 2.0 || Viewport().ActualHeight() < 2.0)
            {
                RequestRender();
                return;
            }

            double scale = 1.0;
            if (auto const root = Viewport().XamlRoot())
            {
                scale = root.RasterizationScale();
            }

            auto const width = static_cast<std::uint32_t>(
                std::max<long>(1, std::lround(Viewport().ActualWidth() * scale)));
            auto const height = static_cast<std::uint32_t>(
                std::max<long>(1, std::lround(Viewport().ActualHeight() * scale)));

            m_renderer->Resize(width, height);
            if (!m_renderer->IsReady()) return;

            DirectX::XMMATRIX const view = m_camera.View();
            UpdateAxisGizmo(view);
            DirectX::XMMATRIX const projection = m_camera.Projection(
                static_cast<float>(width) / static_cast<float>(height));

            // 棱柱固定在点云中：不再随视角变化；只有选中后拖动/滚轮/长度滑杆才会改变它

            DirectX::XMFLOAT4X4 viewProjection{};
            DirectX::XMStoreFloat4x4(&viewProjection, DirectX::XMMatrixMultiply(view, projection));

            // 世界坐标轴示意线：以当前点云包围盒中心为原点，延伸到“无限远”（由裁剪平面截断）
            if (!m_cloud.vertices.empty())
            {
                DirectX::XMFLOAT3 const center{
                    (m_cloud.boundsMin.x + m_cloud.boundsMax.x) * 0.5f,
                    (m_cloud.boundsMin.y + m_cloud.boundsMax.y) * 0.5f,
                    (m_cloud.boundsMin.z + m_cloud.boundsMax.z) * 0.5f };
                constexpr float kInfiniteAxisLength = 1.0e6f;
                m_renderer->SetAxes(center, kInfiniteAxisLength, m_axesVisible);
            }
            else
            {
                m_renderer->SetAxes({}, 0.0f, false);
            }

            float const pointSize = static_cast<float>(PointSizeSlider().Value()) * static_cast<float>(scale);
            m_renderer->Render(viewProjection, pointSize);
        }
        catch (std::exception const& error)
        {
            if (!m_renderFailureReported)
            {
                m_renderFailureReported = true;
                ShowError(L"渲染失败：" + Utf8ToWide(error.what()));
            }
        }
    }

    // -----------------------------------------------------------------------
    // 界面状态
    // -----------------------------------------------------------------------
    void MainWindow::UpdateStats()
    {
        if (m_cloud.loadedVertexCount == 0)
        {
            StatsText().Text(L"尚未加载点云");
            return;
        }

        std::wstring text = FormatCount(m_cloud.loadedVertexCount) + L" 点";
        if (m_cloud.hasColors) text += L" · 顶点颜色";
        if (m_cloud.hasNormals) text += L" · 法线着色";
        StatsText().Text(text);
    }

    void MainWindow::SetBusy(bool busy)
    {
        m_busy = busy;
        BusyRing().IsActive(busy);
        BusyRing().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        OpenButton().IsEnabled(!busy);
        DemoButton().IsEnabled(!busy);
    }

    void MainWindow::SetStatus(std::wstring const& text)
    {
        StatusText().Text(text);
    }

    void MainWindow::ShowError(std::wstring const& message)
    {
        SetStatus(L"⚠ " + message);
        if (m_cloud.loadedVertexCount == 0)
        {
            EmptyHint().Text(message);
            EmptyHint().Visibility(Visibility::Visible);
        }
    }
} // namespace winrt::PointCloudEditor::implementation
