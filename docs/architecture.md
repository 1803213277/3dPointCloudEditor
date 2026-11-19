# 架构与渲染实现

## 代码结构

| 文件 | 作用 |
| --- | --- |
| `3dPointCloudEditor/MainWindow.xaml` | 界面：菜单栏、工具栏、悬浮点云列表面板、`SwapChainPanel` 视口、状态栏 |
| `3dPointCloudEditor/MainWindow.xaml.cpp` | 文件打开/拖放、指针交互、渲染循环、状态提示、多片点云管理 |
| `3dPointCloudEditor/PointCloudRenderer.h/.cpp` | DirectX 12：设备、交换链、根签名、PSO、顶点上传、绘制、聚类高亮 |
| `3dPointCloudEditor/PlyLoader.h/.cpp` | PLY / PCD 解析与保存（编码、大小端、颜色/法线/强度、LZF 解压） |
| `3dPointCloudEditor/OrbitCamera.h` | 自由视角相机：自动取景、转视角、平移、前进/后退 |
| `3dPointCloudEditor/SelectionVolume.h` | 选择平面、多边形内外、棱柱内外判定 |
| `3dPointCloudEditor/AppContext.h` | 主窗口 HWND（文件选择器需要） |
| `3dPointCloudEditor/pch.h` | 预编译头，集中引入 D3D12/DXGI/DirectXMath/交互头文件 |

## DirectX 12 实现要点

1. **设备与队列**：枚举 `IDXGIAdapter1`，优先选择显存最大的硬件适配器；若都不支持
   D3D12 则回退到 WARP。创建 `ID3D12Device` + `ID3D12CommandQueue` + 围栏同步。
2. **呈现**：`IDXGIFactory2::CreateSwapChainForComposition` 创建
   `B8G8R8A8_UNORM`、`FLIP_SEQUENTIAL`、2 缓冲的交换链，再通过
   `ISwapChainPanelNative::SetSwapChain` 绑定到 XAML 的 `SwapChainPanel`；
   按 `RasterizationScale()` 使用物理像素尺寸，`ResizeBuffers` 处理窗口缩放。
3. **管线**：单根参数（根常量 `b0`：视图投影矩阵 + 点大小 + 视口尺寸）、
   `POINTLIST` 拓扑、`D32_FLOAT` 深度缓冲、无混合、无背面剔除。
4. **点精灵**：顶点着色器只做坐标变换；几何着色器把每个点扩成屏幕对齐的四边形
   （`pointSize` 像素，透视校正）；像素着色器裁成圆形并做边缘变暗。
5. **上传**：顶点数据写入 `UPLOAD` 堆后 `CopyBufferRegion` 到默认堆，用围栏等待 GPU 完成；
   点数超过 1600 万时按块拆分到多个顶点缓冲区，绘制时逐块
   `DrawInstanced`；界面空闲时按需重绘（单次 16 ms 计时器合并重绘请求）。
6. **聚类选择**：点击点云后以射线最近点为种子，在“范围”滑杆指定的窗口内按“邻接距离”做区域生长；
   选中点复制到一份**红色点顶点缓冲**里覆盖绘制（与主点云同位置、深度 `LESS_EQUAL`），
   拖滑杆只重建这份小缓冲；删除时在 CPU 端过滤顶点后重新上传。
   **为了支持删除，加载后会保留 CPU 顶点副本**。
7. **健壮性**：着色器编译、设备创建、交换链与绘制错误都会转换成中文提示显示在状态栏，
   设备丢失（`DXGI_ERROR_DEVICE_REMOVED`）也会给出提示。

## 坐标系约定

- 世界坐标固定为 **Z 轴向上**，与 CloudCompare / PCL 等点云工具一致。
- 渲染使用**右手坐标系**（`XMMatrixLookAtRH` / `XMMatrixPerspectiveFovRH`），
  与 MeshLab、CloudCompare 的显示习惯一致，不会出现左右镜像。
- 默认视角为正视图（略微俯视）：X 轴朝屏幕右侧、Z 轴朝上；打开文件或点“重置视角”都会回到该视角。
- 视口里从当前点云中心沿 ±X（红）、±Y（绿）、±Z（蓝）画**无限延伸**的坐标轴示意线，
  正轴颜色鲜艳、负轴较浅；右下角另有 2D 坐标轴指示器。

## 多片点云

每次“打开点云”或“示例点云”都会把新点云加入左侧列表，每项前有勾选框：

- 勾选即可**同时显示**多片点云（各自独立的顶点缓冲，逐片 `DrawInstanced`）；
- 点击名称切换“当前点云”（选中行蓝色高亮，列表顺序保持加载顺序）；
- 选择、删除、平移旋转等编辑操作只作用于**当前点云**。

## 主题与语言

- 启动时读取系统浅色/深色设置，界面配色、面板、文字和 3D 视口背景对应切换；
  运行中在 Windows 设置里切换主题会**实时更新**（订阅 `UISettings.ColorValuesChanged`）。
- 菜单“主题”可手动指定 跟随系统 / 浅色 / 深色；菜单“语言”切换中文 / English，
  所有界面文案（含文件对话框和“关于”）即时更新。
