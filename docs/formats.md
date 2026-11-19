# PLY / PCD 支持细节

## PLY

- **编码**：`ascii`、`binary_little_endian`、`binary_big_endian`。
- **顶点属性**（顺序任意）：
  - 必需：`x` `y` `z`
  - 可选颜色：`red` `green` `blue`（或 `diffuse_red/green/blue`），支持 `uchar` 0–255 与 `float` 0–1
  - 可选灰度：`intensity` / `scalar` / `gray`
  - 可选法线：`nx` `ny` `nz`（存在时按法线做漫反射着色）
- 没有颜色也没有强度时，自动按 Z 高度使用 Turbo 伪彩色显示。
- 其它元素（如 `face`）会被跳过——本程序按点云显示顶点，不渲染三角面片。

## PCD（PCL 格式）

- **编码**：`ascii`、`binary`、`binary_compressed`（内置 LZF 解压，无需第三方库）。
- **字段**（按名字匹配，顺序任意）：
  - 必需：`x` `y` `z`
  - 可选颜色：`rgb` / `rgba`（4 字节，浮点或整数位模式均可）
  - 可选法线：`normal_x` `normal_y` `normal_z`
  - 可选灰度：`intensity`（float 0–1 或整型 0–255/0–65535）
- 支持 `WIDTH` / `HEIGHT` / `POINTS` 与任意 `COUNT`（多余分量会被忽略）。
- 没有颜色/强度时同样按 Z 高度伪彩色显示。

## 保存

| 菜单 | 输出 |
| --- | --- |
| `保存 PLY…` | `binary_little_endian` PLY，含 `x/y/z`、`nx/ny/nz`、`red/green/blue` |
| `保存 PCD…` | `binary` PCD，含 `x/y/z`、`rgb`、`normal_x/y/z` |

保存对话框默认打开“文档”文件夹，使用 Win32 文件对话框（`IFileSaveDialog`），
以管理员身份运行时也能正常弹出。保存会写入当前点云的编辑结果（删除过的点不会回来）。

## 点数与内存

- **点数不设固定上限**：只受可用内存/显存限制。顶点按每 1600 万个一组分块上传，
  绘制时逐块 `DrawInstanced`，因此可以突破 D3D12 顶点缓冲区视图 32 位大小的限制；
  加载超出内存时会提示“内存不足”，显存不足时会提示对应的 HRESULT。
- 为了支持聚类选择/删除，加载后会**保留一份 CPU 顶点副本**，内存占用约 28 字节/点。

## 测试数据

`samples/` 下的 4 个文件覆盖了上面所有编码路径，可在程序里用工具栏“示例点云 ▾”直接打开：

| 文件 | 说明 |
| --- | --- |
| `samples/sphere_ascii.ply` | ASCII，带颜色和法线的球体（1 万点） |
| `samples/wave_binary_le.ply` | 二进制小端，带颜色的波浪曲面（9 万点） |
| `samples/helix_binary_be.ply` | 二进制大端，只有坐标（按高度伪彩色，6 千点） |
| `samples/torus.pcd` | PCD ASCII，带颜色的环面（2 万点） |

`tests/data/` 里另有 4 个小 PCD（ascii / binary / binary_compressed / 保存结果），
用于手工验证解析与保存。
