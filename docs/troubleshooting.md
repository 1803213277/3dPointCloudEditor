# 排查手册

## 双击 exe 没反应 / 提示缺 DLL

程序是**非打包 + 自包含**应用，Windows App SDK 运行时 DLL 都在输出目录里，但：

- 必须**整个目录**一起拷贝，只拷 `3dPointCloudEditor.exe` 会缺依赖；
- 目录里**不含 VC++ 运行库**。若提示缺少 `MSVCP140.dll` / `VCRUNTIME140.dll`，
  装一次 [VC++ 可再发行组件](https://aka.ms/vs/17/release/vc_redist.x64.exe)，
  或手动把 `Microsoft.VC145.CRT` 下的 `msvcp140*.dll`、`vcruntime140*.dll` 拷进该目录。

## 应用启动后只有黑窗 / 直接退出

非打包应用不需要“应用包身份”，出现这种情况通常是运行环境问题：

- 确认是**整个输出目录一起**运行（`x64\Release\` 里有 100 多个 DLL）；
- 旧版本曾以 MSIX 方式安装过的话，先卸载旧包，避免开始菜单磁贴启动到旧版本：
  ```powershell
  Get-AppxPackage -Name 76abcfdd-2b18-41e5-a00e-9aeec08dbae4 | Remove-AppxPackage
  ```
- 用命令行启动 `x64\Release\3dPointCloudEditor.exe` 观察是否有错误弹窗。

## 提示“DirectX 12 初始化失败”

确认显卡与驱动支持 D3D12（`dxdiag` 检查），或查看状态栏里的 HRESULT。
无独显的机器会自动回退到 WARP 软件渲染，此时帧率较低但可以正常显示。

## 打开文件后没有画面

- 确认点云坐标不是极端大/小（程序按包围盒自动取景），可点“重置视角”；
- 确认文件里确实有 `x y z` 顶点；只有 `face` 的面片文件不会显示任何东西。

## 保存时提示“你没有权限在此位置中保存文件”

这是 Windows 对**目标文件夹**的写权限检查（也可能来自受限/低完整性级别的进程），不是点云数据的问题：

- 保存对话框默认打开“文档”文件夹；请选择“文档”“桌面”或“下载”等可写目录；
- 不要保存到 `C:\Program Files`、`C:\Windows` 或只读的网络/OneDrive 文件夹；
- 确认应用是以普通用户方式启动的（双击 `x64\Release\3dPointCloudEditor.exe` 或 VS `F5`）；
- 程序使用 Win32 的 `IFileSaveDialog` / `IFileOpenDialog`（而不是 WinRT 的 Picker），
  在“以管理员身份运行”的进程里也能正常弹出文件对话框。

## 示例点云菜单提示“未找到示例文件”

“示例点云 ▾”读取的是 exe 同目录下的 `samples\`：

- 构建时会自动把仓库根的 `samples\` 复制到输出目录；
- 如果只拷了 exe，会退回程序内置的 15 万点彩色环面，仍然可以体验渲染；
- 手工恢复：把 `samples\` 整个目录拷到 exe 旁边。

## 编译时报 MakePri.exe 退出代码 -2147024891（0x80070005）

这是 `Microsoft.Windows.SDK.BuildTools 10.0.28000.x` 自带的 32 位 `makepri.exe`
在部分 Windows 11 版本上以“拒绝访问”退出导致的（同样的命令改用 Windows SDK 自带的
`makepri.exe` 可以成功）。项目里已经处理：在 `3dPointCloudEditor.vcxproj` 中把
`MakePriExeFullPath` 指向当前安装的 Windows SDK：

```xml
<MakePriExeFullPath Condition="Exists('$(WindowsSdkDir)bin\$(TargetPlatformVersion)\x86\makepri.exe')">$(WindowsSdkDir)bin\$(TargetPlatformVersion)\x86\makepri.exe</MakePriExeFullPath>
```

如果仍有问题，可以升级 `Microsoft.Windows.SDK.BuildTools` NuGet 包，或确认
`C:\Program Files (x86)\Windows Kits\10\bin\<版本>\x86\makepri.exe` 存在。

## 编译报找不到 App.xaml.g.h / 界面控件名“找不到标识符”

工程的 `ApplicationType` / `ApplicationTypeRevision` / `EnableMsixTooling` 三个属性**不能删**：
Visual Studio 的 C++ XAML 工具链靠它们挂接 XAML 编译（生成 `App.xaml.g.h`）和 `resources.pri`。
它们只影响构建管线，**是否打包成 MSIX 只由 `WindowsPackageType` 和 `AppxPackage` 决定**。
