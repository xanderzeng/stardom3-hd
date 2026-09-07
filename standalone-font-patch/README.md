# Stardom 3 Font Patch

《明星志愿 3》独立字体补丁，提供字体替换、描边合成与文字对比度修正，保留原版页面布局和窗口尺寸。

## 功能

- 将游戏通过 `CreateFontIndirectA` 请求的 `MingLiU` 替换为可配置字体。
- 默认对高度不超过 16px 的小字使用宋体 `SimSun`，其余文字使用黑体 `SimHei`。
- 小字和普通字体可分别配置字体与缩放比例，也可关闭按字号分流。
- 其他字体请求保持不变。
- 与宽屏版复用同一份字体描边代码，普通字和小字均合并四向描边覆盖率。
- 12/16px 等小字号的浅色字心使用细深色描边；物品说明和存读档浅色文字也有定向对比度修正。
- 剧情对话正文的灰白字提亮为白色并加细深色描边，保留字号、透明度和换行布局。
- 接入 D3D9 的四种字形绘制接口，绘制后恢复状态；不改变设备创建参数、游戏分辨率和 UI 布局。

> **适用范围：** 字体替换目前只对游戏简体模式生效，不会替换繁体模式使用的字体。

## 使用

1. 将 `release/d3d9.dll` 和 `release/Stardom3.FontPatch.ini` 复制到 `Stardom3.exe` 所在目录。
2. 确保同一目录没有其他名为 `d3d9.dll` 的代理或补丁。
3. 正常启动游戏。

默认配置：

```ini
[FontPatch]
Enabled=1
FontOutlineUnion=1
FontName=SimHei
FontScale=1.00
SmallFontName=SimSun
SmallFontMaxHeight=16
SmallFontScale=1.00
```

`FontScale` 与 `SmallFontScale` 的有效范围均为 `0.75`–`1.50`；设置
`SmallFontMaxHeight=0` 可关闭小字号分流。游戏使用固定字形格，建议缩放保持
`1.00`，以免裁字或串字。

`FontOutlineUnion=0` 可关闭描边和对比度修正，`Enabled=0` 关闭全部字体修正。
小字体可选 `SmallFontName=SimHei`，描边修正不依赖字体名称，不会自动改回宋体。
原生入口签名不匹配时保留普通字体替换，不修改未知游戏代码。独立版不输出开发诊断。

## 构建

请使用 Visual Studio 2022 x86 工具链：

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

游戏是 32 位程序，不要生成 x64 DLL。

构建需要保留仓库中的共享 `../src/font_outline*`、`../src/gui_object*` 和测试源码。
