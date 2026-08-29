# Stardom 3 Font Patch

《明星志愿 3》独立字体替换补丁。该项目不包含宽屏、UI 布局、视频、渲染或窗口修改。

## 功能

- 将游戏通过 `CreateFontIndirectA` 请求的 `MingLiU` 替换为可配置字体。
- 默认对高度不超过 16px 的小字使用宋体 `SimSun`，其余文字使用黑体 `SimHei`。
- 小字和普通字体可分别配置字体与缩放比例，也可关闭按字号分流。
- 其他字体请求保持不变。
- 原样转发系统 `Direct3DCreate9` / `Direct3DCreate9Ex`，不包装 D3D9 对象。

> **适用范围：** 字体替换目前只对游戏简体模式生效，不会替换繁体模式使用的字体。

## 使用

1. 将 `release/d3d9.dll` 和 `release/Stardom3.FontPatch.ini` 复制到 `Stardom3.exe` 所在目录。
2. 确保同一目录没有其他名为 `d3d9.dll` 的代理或补丁。
3. 正常启动游戏。

默认配置：

```ini
[FontPatch]
Enabled=1
FontName=SimHei
FontScale=1.00
SmallFontName=SimSun
SmallFontMaxHeight=16
SmallFontScale=1.00
```

`FontScale` 与 `SmallFontScale` 的有效范围均为 `0.75`–`1.50`；设置
`SmallFontMaxHeight=0` 可关闭小字号分流。游戏使用固定字形格，建议缩放保持
`1.00`，以免裁字或串字。

## 构建

请使用 Visual Studio 2022 x86 工具链：

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

游戏是 32 位程序，不要生成 x64 DLL。
