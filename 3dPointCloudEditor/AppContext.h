#pragma once

#include <windows.h>

namespace app
{
    // 主窗口句柄，在 App::OnLaunched 中赋值。
    // 文件选择器（IInitializeWithWindow）等 WinRT API 需要它。
    inline HWND MainWindowHandle{ nullptr };
}
