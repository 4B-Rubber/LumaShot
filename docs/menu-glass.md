# 菜单与设置窗口的玻璃效果（2026-10-20）

用户问题：设置窗口不如 full.html 通透，贴图和托盘的右键菜单是实心的。处理方案为“两个都改”。

## 原因

| 窗口 | 修改前 | 实际不透明度 | full.html |
|---|---|---|---|
| 设置 | DWM 旧版 acrylic 色调 70%，再叠加一层 D2D .60/.72 底色 | 约 88% / 92% | `.glass` rgba(247,249,252,.62) / rgba(37,44,56,.72) + blur(30px) |
| 贴图菜单、托盘菜单 | 不透明 D2D 底色 | 100% | `.menu.glass` |
| 剪贴板面板 | Win11 系统背景（类型 3）+ .60/.72 色调 | 与原型一致 | — |

## 设置窗口

- `SetSettingsAcrylic`：DWM 色调改为令牌值 `glass::Opacity(dark)`（亮 60% / 暗 72%），色值 `glass::Fallback`。
- `settings_dialog.cpp`：开启 acrylic 时自己的底色改为 1/255（只给 DWM 留 alpha），色调只有一层。
- 关闭透明效果、高对比度时仍是不透明底色。
- 验证：`lumashot_settings_test` 646 项通过；合成背景预览 `build\settings-light-p0.png`、`build\settings-dark-p0.png`。

## 贴图菜单与托盘菜单

最终方案与剪贴板面板相同：

1. **Win11 系统背景**：`ApplyPopupBackdrop(HWND,bool dark)`（`src/ui/acrylic.cpp`）设置 `DWMWA_SYSTEMBACKDROP_TYPE = 3`（瞬态 acrylic）、深色模式属性、圆角偏好，并把帧扩展到整个客户区。
2. **窗口样式**：`WS_POPUP | WS_THICKFRAME`，`WM_NCCALCSIZE` 返回 0，`WM_NCHITTEST` 返回 `HTCLIENT`。DWM 只给带框样式的窗口画系统背景：仅用 `WS_POPUP` 时整块是灰色兜底色。无边框、不可调整大小。
3. **保持激活外观**：Win11 对非激活窗口显示纯色兜底。菜单可能拿不到前台（防抢焦点规则），所以 `WM_NCACTIVATE` 一律按激活处理，显示后再发送一次 `WM_NCACTIVATE TRUE`。
4. **内容**：窗口使用 `WS_EX_NOREDIRECTIONBITMAP`，经 `ClipboardComposition` 以预乘 BGRA 呈现。底色为 Luma Glass 色调 `PanelBackground(dark)` × `glass::Opacity(dark)`，铺满整个矩形，让 DWM 的圆角也带色调；悬停使用 accent-soft，不画描边。
5. **阴影**：独立的点击穿透分层窗口 `LumaShot.PopupShadow`，挖空 8 px 圆角主体，放在菜单下方。点击阴影区等同点击菜单外。
6. **坐标**：贴图菜单窗口与主体同大，命中测试使用 `PinMenuInset(HWND)`；托盘菜单使用 `p->origin`。
7. **兜底**：关闭透明效果时仍是原来的分层窗口、不透明底色加自绘阴影。

### 走过的弯路

- 旧版 accent acrylic + 分层窗口 + `SetWindowRgn`：DWM 忽略区域，模糊整个窗口矩形，阴影留白也变成毛玻璃。已放弃。
- 系统背景配纯 `WS_POPUP`：显示灰色兜底（亮约 #d3d3d3，暗约 #555）。需要加 `WS_THICKFRAME`。
- 没有色调：Win11 瞬态 acrylic 很薄，比原型更透、更艳，深色主题文字不易读。所以加 .60/.72 令牌色调（与剪贴板面板一致）。

## 验证

- `lumashot_pin_menu_test`：61 项通过。真实玻璃菜单在合成彩条背景上截图，测量主体通道极差：亮 64 / 暗 52（修改前为 0），菜单旁锐利边缘 1920 / 1920（阴影窗口不模糊背景）。预览 `docs/pin-menu-glass-light.png`、`docs/pin-menu-glass-dark.png`。
- `lumashot_tray_menu_test`：199 项通过。
- `ocr_option` 通过；`recording_ui` 单独运行 241 项通过（ctest 中因 CPU 满载超时）。
- `pin_interaction`（`lumashot_pin_test`）54 项通过；2 项 OCR 时序断言在 CPU 100% 负载下失败，属已知负载敏感项，与本次修改无关。
- 修改脚本留档在会话工作区 `phase8/edits_glass*.py`。

## 录制界面（同日，第二轮）

用户反馈：录制界面也“都不透”。录制面板、共享下拉菜单（录制的质量 / 尺寸 / 帧率选择，设置窗口也在用）和主题确认框（如“关闭录制？”）原先都是“分层窗口 + 旧版 accent acrylic”。暗色色调约 72%，又没有亮度和饱和度层，看起来几乎是实心。

改为与菜单相同的方案：

| 窗口 | 文件 | 改动 |
|---|---|---|
| 录制面板 | `src/recording/worker.cpp`、`panel.h/.cpp` | 透明效果开启时为非分层窗口，`WS_THICKFRAME` + `WS_EX_NOREDIRECTIONBITMAP`，`ApplyPopupBackdrop`。DIB 经 `ClipboardComposition` 呈现。`PanelState::backdrop` 让面板画 Luma Glass 色调（`DrawBackdropTint`），不再画 1/255 底和描边。`WM_NCACTIVATE` 始终按激活处理。标题栏拖动（`HTCAPTION`）不变，其余区域为 `HTCLIENT`。 |
| 共享下拉菜单 | `src/ui/dropdown_window.cpp` | 同上；创建后按精确尺寸再设置一次。 |
| 主题确认框 | `src/ui/themed_message.h` | 同上，并新增 `WM_NCCALCSIZE` 返回 0。 |
| 共用 | `src/ui/glass_surface.h`、`src/ui/acrylic.cpp` | 新增 `DrawBackdropTint`；`acrylic.cpp` 用 `#pragma comment` 链接 d3d11 / dxgi / dcomp（录制 worker 和测试目标也编译此文件，因此不改 CMake）。 |

- `WS_THICKFRAME` 会让系统强制最小窗口尺寸（约 136×36 px），较窄的下拉菜单会被撑大。三个窗口过程都在最前面（`WM_NCCREATE` 之前也生效）处理 `WM_GETMINMAXINFO`，最小尺寸设为 1×1。
- 关闭透明效果时仍走原来的分层窗口，使用不透明底色。
- 测试中的录制面板窗口由测试自己创建（分层），仍覆盖旧路径。

验证：

- `lumashot_themed_message_test` 45 项通过。新增真实玻璃截图：生产环境窗口设置了 `WDA_EXCLUDEFROMCAPTURE`，测试里临时解除。主体通道极差亮 63 / 暗 52，与菜单（64 / 52）一致。预览 `docs/themed-message-glass-light.png`、`docs/themed-message-glass-dark.png`。
- `lumashot_recording_ui_test` 241 项通过：真实下拉菜单的选择、Esc 和点外关闭都正常。断言“无非客户区边框”改为：玻璃模式允许 `WS_THICKFRAME`，但客户区必须等于窗口矩形。
- `lumashot_recording_result_test` 18 项通过（CPU 满载时导出超过 90 秒会超时，空闲后重跑通过）。
- `lumashot_settings_test` 646 项通过。
- 尚未做：真实录制面板（worker 进程）的玻璃截图。它与确认框、下拉菜单用同一套代码，需要在真机上确认。

## 长截图工具条与预览面板（同日，第三轮）

- `src/longshot/session.cpp/.h`：透明效果开启时，工具条和预览面板为非分层 `WS_THICKFRAME` + `WS_EX_NOREDIRECTIONBITMAP`，使用 `ApplyPopupBackdrop`。选区边框仍是分层点击穿透窗口。预览面板 `WS_EX_NOACTIVATE`，从不激活，工具条也会把焦点让给被滚动的程序，所以显示后发送 `WM_NCACTIVATE TRUE`，并且始终按激活处理。`WM_GETMINMAXINFO` 在 `Session::Proc` 最前面处理，最小尺寸为 1×1。
- `src/longshot/canvas.h/.cpp`：`LayeredCanvas::Paint` 看到 `WS_EX_NOREDIRECTIONBITMAP` 时，用 `SetWindowPos` 移动和调整窗口（原先由 `UpdateLayeredWindow` 顺带完成）。DIB 行宽补成偶数，奇数宽度时先压紧再交给 `ClipboardComposition`。`Painter::Backdrop(b, acrylic, tint)`：tint 为真时画 Luma Glass 色调。
- 验证：`ctest -R ^long_capture` 6/6 通过（stitch、session 端到端、viewer、annotation、annotate_editor、memory）。
- 注意：`lumashot_long_capture_test --preview` 会截到真实桌面（会话窗口周围的内容）。截图已立即删除；请勿用它做玻璃验证，需要时应先改成合成背景。

## 设置窗口（同日，第四轮）

- `src/app/resources.rc`：对话框模板 100 增加 `WS_THICKFRAME` 和 `WS_EX_NOREDIRECTIONBITMAP`（写成 0x00200000）。`WS_EX_NOREDIRECTIONBITMAP` 只能在创建时设置，所以写在模板里。
- `src/app/settings_dialog.cpp`：
  - 不再调用 `ConfigureBorderlessWindow`（它会加 `WS_EX_LAYERED`）和 `ResizeBorderlessWindow`；圆角由 DWM 偏好设置（33 = 2）提供。
  - `ApplyPopupBackdrop` 取代 `SetSettingsAcrylic`，切换明暗主题时重新设置。设置成功后发送 `WM_NCACTIVATE TRUE`；`WM_NCACTIVATE` 始终按激活处理，失去焦点时也保持玻璃。
  - 帧经 `ClipboardComposition` 呈现，透明效果开启和关闭时都走这条路；关闭时不设背景，底色不透明。
  - 底色为令牌色调 60% / 72%，铺满整个矩形；侧栏阴影也铺满整个矩形。`WM_NCHITTEST` 默认返回 `HTCLIENT`（顶部仍为 `HTCAPTION`）。
- 测试（`lumashot_settings_test` 614 项，0 失败）：
  - “无系统标题栏 / 边框”改为允许 `WS_THICKFRAME`，但要求 `WS_EX_NOREDIRECTIONBITMAP`，且客户区等于窗口矩形（原有检查）。
  - “四角全透明”改为“底色铺到角落”：DWM 负责圆角，角落透明会在系统圆角下露出空隙。
- `lumashot_settings_coexist_test` 16 项通过（第一次运行时另一程序占着前台，“获得前台”失败；重跑通过）。
- 曾在设置测试里临时加过真实屏幕截图（测试背景窗口放在设置窗口下方）。设置窗口不是置顶窗口，用户的其他窗口可能盖在上面，结果截到了用户桌面上的其他程序窗口。截图已立即删除，相关测试代码已移除。删除前的一次有效截图（未被遮挡）确认了真实毛玻璃效果（侧栏列极差 61）。今后只在置顶窗口上做真实截图（菜单、确认框）。

## 快速剪贴板窗口（同日，第五轮）

- `src/clipboard/quick_window.cpp`：
  - 窗口样式改为 `WS_POPUP | WS_THICKFRAME`（扩展样式不变，已有 `WS_EX_NOREDIRECTIONBITMAP` 和 DComp 呈现）。
  - `WM_NCCALCSIZE` 返回 0，`WM_NCHITTEST` 返回 `HTCLIENT`，`WM_GETMINMAXINFO` 把最小尺寸设为 1×1。
  - 改用 `ApplyPopupBackdrop`。窗口带 `WS_EX_NOACTIVATE`，永远不会被激活，所以设置背景后、每次显示后都发送 `WM_NCACTIVATE TRUE`，`WM_NCACTIVATE` 始终按激活处理，否则会显示灰色的非激活材质。这样做不会改变前台、焦点或激活状态（有测试覆盖）。
  - 毛玻璃模式下底色为令牌色调 60% / 72%，铺满整个矩形；不再自己画描边，由 DWM 画圆角和描边。不支持时仍用不透明圆角底色加描边。
- 测试 `lumashot_clipboard_quick_window_test`（54 项通过，0 失败）：
  - 新增“采用 DWM 系统背景窗口方案”（`WS_THICKFRAME` 加 `WS_EX_NOREDIRECTIONBITMAP`）和“客户区等于窗口”。
  - “系统毛玻璃上不加色调层”改为“系统毛玻璃带 Luma 令牌色调”（浅色透明度在 140–200）。
  - ctest `clipboard_quick_window` / `clipboard_quick_input` 都通过。
- 这一轮没有截取真实屏幕；视觉预览来自离屏渲染图，叠在合成的条纹背景上。截图：`quick-clipboard-glass-light.png`、`quick-clipboard-glass-dark.png`。

## 截图工具栏（软件模拟亚克力，同日）

截图工具栏、属性面板、取色器和尺寸标签都画在截图遮罩窗口内部，盖在冻结的截图上，所以不能用 DWM 亚克力。它们把冻结截图模糊后，取模糊图里对应的区域作为背景（`BlurBackdrop`，经 `Renderer::Panel` 绘制）。

改动前不通透的原因：
- 浅色叠色 77%（深色 66%）；
- 先按 1/4 点采样缩小，再只做一次横向和一次纵向的 9 点盒式模糊，结果块状、发灰；
- 没有饱和度增强。

改动（`src/ui/render.cpp`）：
- `BlurBackdrop`：
  - 先按 4×4 面积平均缩小；
  - 再做 3 轮“7 点横向 + 7 点纵向”盒式模糊，效果接近高斯模糊，全分辨率下半径约 14 px；
  - 最后饱和度 ×1.3（`AcrylicSaturate`），和 Fluent 亚克力一样，让颜色透得出来。
  - 全部用整数运算。R、B 两个通道放在一个 32 位字里一起累加；纵向模糊按行扫描，每列维护一个滑动累加和。
- `Panel` 叠色：浅色改为 50%（`0x80EAF3FF`），深色改为 58%（`0x9423354B`）。

测试：
- `startup_render_pixels`：参考实现换成新算法的朴素写法，与优化版逐像素一致。新增耗时上限：1080p 40 ms，4K 120 ms。实测 1080p 7 ms、4K 30 ms（参考实现 25 / 110 ms），截图启动在 1080p 下约多 4 ms。
- `text_context_memory`、`text_export_memory`、`render_optimization`、`toolbar_motion` 都通过。

预览：`LumaShot.exe --render-demo[-dark]`，合成画面，截图为 `toolbar-acrylic-light.png` / `toolbar-acrylic-dark.png`。

## 贴图标注工具栏（同日）

贴图标注（以及长图标注）复用截图会话和截图工具栏。以前它的背景帧是 `PinAnnotationPreview` 生成的：整块浅灰底（`0xF4F7FB`）上贴上贴图。工具栏的软件亚克力模糊的是这块浅灰底，所以看起来是一块不透明的浅灰板，和截图时的工具栏不一样。

改动（用户选择“进入标注时在内存里截一次当前屏幕当背景”）：
- `PinAnnotationPreview(source, display, canvas, const Frame* backdrop = nullptr)`：传入的屏幕副本和画布尺寸一致时，以它为底，再贴上贴图；否则仍用浅灰底。
- `Application::AnnotatePin`：用 `CaptureRegion(monitor.rcWork)` 截一次当前显示器工作区，只放在内存里，不保存、不写盘，会话结束即丢弃。演示 / 诊断会话不截屏，保持结果确定。截屏失败时退回浅灰底。
- 测试 `pin_annotation` 7 项通过，新增两项：屏幕副本填充贴图周围；尺寸不匹配时退回浅灰底。
- 注意：`long_annotate_app_test` 原本就会截取真实屏幕并保存 PNG，开发时不要在用户桌面上运行它。

### 贴图标注工具栏四角（续）

问题：四角不干净。原因有两个：
- `UpdatePinEditRegion` 用 `CreateRoundRectRgn`（半径 20×缩放）裁剪窗口，GDI 区域没有抗锯齿，有锯齿；
- 区域圆角和工具栏绘制的圆角半径不一致，圆角外露出选区外的变暗遮罩，阴影也被区域切掉。

方案（和截图模式四角干净的原理相同：圆角外就是冻结的屏幕画面）：
- 新增 `ViewState::clear_outside`。贴图标注且有屏幕副本时，`Renderer::Chrome` 不画选区外的遮罩，也不画尺寸标签。
- `UpdatePinEditRegion`：`clear_outside` 时，工具栏、取色器、下拉框用矩形区域，四周留 `ceil(6×缩放)` 的边距（工具栏与选区间距是 10×缩放，阴影最多向下偏移 4 px）。边距里显示的正是刚截的屏幕副本，抗锯齿圆角和阴影都和真实桌面融为一体。没有屏幕副本时（截屏失败、演示会话）保持原来的圆角区域。
- 局限：标注期间如果工具栏后面的内容在变化（比如视频），边距这一圈显示的是进入标注时的画面。
- 测试 `toolbar_motion` 148 项通过，新增 4 项：选区外不变暗；没有尺寸标签；工具栏圆角外的边距就是后面的屏幕；工具栏仍然正常绘制。`pin_annotation`、`startup_render_pixels`、`render_optimization` 都通过。
### 贴图标注：黑边和提示被遮挡（续）

用户截图里有两个问题：工具栏左下方有一块黑色矩形；“矩形”按钮的悬停提示只露出一部分，像是被贴图的边框挡住。

原因：
- **黑边**：从贴图右键菜单点“标注”时，`TrackPinMenu` 刚 `DestroyWindow` 菜单就返回，`Annotate` 随即同步调用 `CaptureRegion`。这时 DWM 还没合成出没有菜单的一帧，菜单（DWM 亚克力背景在 GDI 截图里读出来是黑的）和它的阴影窗口被截进了屏幕副本。区域边距显示的正是这份副本，于是露出黑块。从悬浮栏按钮或空格键进入时，`Annotate` 也不隐藏悬浮栏，它会作为残影留在副本里。
- **提示**：工具栏提示画在按钮上方约 36 DIP 处（`ui::TooltipBounds`），超出了工具栏 6 px 的区域边距，所以被区域边缘截断。截断的位置正好在贴图下沿外，看起来像被贴图的边框遮住。

改动：
- `PinManager::Annotate`：立即隐藏悬浮栏（不淡出），设置 `WDA_EXCLUDEFROMCAPTURE`，并停止淡入淡出计时器；悬浮栏再次显示时恢复 `WDA_NONE`。
- `TrackPinMenu`：销毁菜单前设置 `WDA_EXCLUDEFROMCAPTURE`。`PopupShadow` 析构时同样处理（托盘菜单也受益）。即使 DWM 还在播放关闭动画，残影也不会出现在截图里。
- `Application::AnnotatePin`：截屏前调用两次 `DwmFlush()`，等 DWM 合成出不含这些弹窗的画面。
- 新增 `ToolbarTooltipReserve(toolbar, hover, property_drag)`，返回当前悬停按钮的提示可能占用的区域：按钮上下各 48 DIP，横向至少 ±170 DIP。`UpdatePinEditRegion` 在 `clear_outside` 时把这块区域加进窗口区域。区域里显示的是屏幕副本（包括贴图自己的边框），提示完整画在最上层，不再被截断。
- `clear_outside` 时选区就是贴图本身，本来就不能拖动调整（`pin_edit_id_` 会屏蔽控制点）。因此边框改为整条画在贴图内侧，也不再画控制点，这样各个区域边距里不会露出半个控制点或边框外半边。普通截图不受影响。

测试：
- `toolbar_motion` 新增 4 项：贴图模式下贴图外没有控制点和外侧边框；边框画在贴图内侧；普通截图仍有控制点；对各个按钮，提示预留区都完整包含实际提示框（`ui::TooltipBounds`）；没有提示时预留区为空。
- `toolbar_motion`、`pin_annotation`、`startup_render_pixels`、`render_optimization`、`pin_zoom_ui`、`pin_styles`、`pin_session`、`capture_to_pin`、`tray_menu` 通过。
- `pin_interaction` 跑了三次，每次失败的项都不一样，都与本次改动无关：剪贴板被其他进程占用（“Clipboard is busy”），以及 OCR 重试时序（机器负载高）。与标注入口相关的几项每次都通过。
- 截取真实屏幕的 `pin_menu` 测试没有运行。
- 安装包：`dist\LumaShot-Setup.exe`，37,998,467 字节，SHA-256 `36EB35B068FB16839C520D3D14FBB12A06B9364E8B7FDF7B9E025B6833DA0D54`。上一版备份为 `dist\LumaShot-Setup-pre-pin-fixes2-20261020.exe`（8E09…）。

## 截图工具栏：展开属性后工具按钮不再拉开（同日）

问题：选中画图工具后，工具栏从 640 DIP 加宽到 944 DIP，为属性行腾出空间。但在“按钮文字=自动”时，工具区把多出来的宽度平均分给 7 个工具，每格从约 39 DIP 变成约 86 DIP：按钮之间空隙很大，选中工具的高亮也变成扁长条。从选择工具切到画图工具时，每个工具按钮的位置和宽度都会跳变。

改动（`PlaceToolbar`）：
- “自动”和“仅图标”都使用图标格（`narrow = labels != Text`）；“图标+文字”仍是 88 DIP 宽格。
- 图标格的工具区宽度统一用选择工具时的值（`compactTools`，每格不超过 40 DIP，默认布局为 272 DIP）。展开属性后工具按钮的位置和大小与选择工具时完全一致，动作按钮仍然靠右，属性行仍是 944 DIP 一行。

测试：`unified_toolbar` 新增两项（缩放 1、1.5、2；矩形、箭头、铅笔）：
- 画图工具下每个工具按钮相对工具栏左边的位置和宽度，与选择工具时一致；
- 动作按钮靠右。

`startup_render_pixels`、`selected_mark_properties`、`selected_property_controls`、`last_toolbar_tool`、`render_optimization`、`toolbar_motion`、`pin_annotation` 都通过。

预览（`--render-demo`，合成画面）：`toolbar-slots-light.png` / `toolbar-slots-dark.png`。
- 安装包：`dist\LumaShot-Setup.exe`，37,994,230 字节，SHA-256 `A25E019263059BBA31E23BC274CC8B8B815AFE9EF7919F484B239B7AF6B6E865`。上一版备份为 `dist\LumaShot-Setup-pre-toolbar-slots-20261020.exe`（36EB…）。

## 截图工具栏 · 方案 C 属性浮卡（2026-10-20）

替代“40 DIP 槽位”方案（用户未采纳）。

- 主工具栏固定 640 DIP（文字工具 944），切换工具时形状、按钮间距都不变。
- 属性移到独立的玻璃浮卡：左边对齐当前工具按钮左缘 −12，与工具栏间隔 8 DIP，内边距 12/10/12，圆角 14。
- 浮卡宽度按内容一行的自然宽度计算，上限 920 DIP（同时不超过显示器宽 −40），超出就换行；位置贴近显示器边缘时夹回 ±8。
- 浮卡放在远离选区的一侧，空间不够时翻到另一侧；展开动画从工具栏一侧开始长出。
- 属性控件只有在浮卡可见时才可用；命中测试用 `Contains(ToolbarLayout, Point)`（工具栏或浮卡）。
- 换行判断加 0.05 px 容差，避免浮卡宽度恰好等于一行宽时因浮点误差被挤成两行（马赛克）。
- 预览：`toolbar-card-light.png` / `toolbar-card-dark.png`。
