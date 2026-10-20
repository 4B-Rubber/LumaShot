# 工具面板自定义与设置页

截图工具栏的常用工具不再固定：用户决定哪些留在面板上、哪些收进末尾的“更多（···）”菜单，以及按钮怎样显示。默认配置与改动前完全一致（没有收起的工具，按钮外观不变）。

## 在哪里改

- 设置 → 工具面板：两个区域“面板上 / “更多”菜单”，12 个工具在任意顺序下都能在两行内放下。点击工具在两边移动，拖动调整顺序（拖动超过系统拖动阈值才算拖动，有插入标记）；键盘焦点在工具上时 Ctrl + ← / → 与相邻工具交换。
- 截图时右键面板上的可自定义按钮 →“收进“更多”菜单”；有收起的工具时还有“全部放回面板”。在“更多”菜单里点行尾的图钉放回面板。截图中的改动立即保存。
- 选择、撤销、重做、保存、复制、取消、完成始终在面板上。可自定义的是：矩形、圆形、箭头、铅笔、文字、马赛克、序号，以及操作区的置顶贴图、识别文字、截图翻译、长截图、录制。
- 置顶贴图收进“更多”后，操作区让出它的位置；从菜单点击或按 F3 仍然可以贴图。
- 当前工具被收进“更多”时，“···”按钮显示该工具的图标并保持选中。

## 显示选项（设置 → 工具面板 → 显示）

- 按钮文字：
  - 自动：原有外观。未选工具时是窄图标按钮，选中工具后按钮加宽。
  - 图标 + 文字：工具按钮显示名称（88 DIP 格宽）。放不下时操作按钮换到第二行；单个按钮仍放不下时只显示图标。
  - 仅图标：选中工具后也保持窄按钮。
- 自动展开属性栏：关闭后，选中工具时属性栏保持收起，工具格末尾出现“属性”按钮（滑块图标，展开时高亮），点击展开或收起，沿用原有的展开动画。每次截图都从收起开始。

## 设置窗口其他改动

- 左侧导航改为 8 页：常规、快捷键、截图、工具面板、剪贴板、贴图、外观、关于。旧版 `SettingsPage` 会映射到新页面。
- 剪贴板页分两组：
  - 历史记录：开启剪贴板、退出后保留历史、最多保留 20 / 50 / 100 / 200 条。调低后立即删除最早的未收藏条目；收藏和分组条目不占这个额度。
  - 侧边条：显示侧边条、样式（卡片堆叠 / 经典细条，保存后立即切换）、卡片显示内容预览。关闭预览后，卡片只显示类型图标和“内容已隐藏”，不加载缩略图。
- 关于页：检查更新、项目主页（github.com/jimmgreen/LumaShot）、第三方许可。“查看”用记事本打开安装目录下的 `THIRD-PARTY-NOTICES.md`；找不到该文件时打开 GitHub 上的同名文件。
- “恢复本页默认”只恢复当前页，第一次点击变成“确认恢复”，并在页脚提示；再点一次才恢复。切换页面或点击其他控件会取消。关于页没有可恢复的设置，不显示该按钮。修改仍需点“保存”才生效。

## 存储

`%LOCALAPPDATA%\LumaShot\settings.ini`：

| 节 / 键 | 含义 |
| --- | --- |
| `[Toolbar] Order` | 可自定义条目的顺序（逗号分隔的工具栏 id）。旧版保存的顺序里没有置顶贴图（13）时，补在第一个快捷操作之前，位置与原来一致 |
| `[Toolbar] Overflow` | 收进“更多”的条目 |
| `[Toolbar] Labels` | 0 自动、1 图标 + 文字、2 仅图标；无效值按自动处理 |
| `[Toolbar] AutoProperties` | 1 自动展开属性栏（默认） |
| `[General] SettingsTab` | 设置窗口上次打开的页（0–7） |
| `[General] ClipboardMaxEntries` | 20 / 50 / 100 / 200，其他值按 100 处理 |
| `[General] ClipboardStripStyle` | 0 卡片堆叠、1 经典细条。缺少时沿用 `clipboard-layout.ini` 的 `[Strip] Style`，并在切换时写回 |
| `[General] ClipboardCardPreview` | 1 显示卡片内容（默认） |

设置窗口在独立进程中运行，偏好通过共享内存传递（`src/app/settings_process.cpp`，版本 19）。工具栏布局和剪贴板选项在宿主进程里再次校验，非法的顺序、重复 id 或未知取值都会被拒绝。

## 代码位置

- 数据模型：`src/model/toolbar_customization.h`。工具栏 id：置顶贴图 = 13（`ToolbarPinId`），“更多”= 19，“属性”= 23，右键菜单 = 90。设置窗口的工具块 id 见 `ChipIds`（贴图 = 175）。
- 布局与绘制：`src/ui/toolbar.cpp`（`PlaceToolbar`、`PanelToolSlots`）、`src/ui/toolbar_render.cpp`
- 截图交互：`src/app/application.cpp`（`ApplyToolbarCustomization`、`OpenToolbarContext`、`PinFromMoreMenu`、`ToggleProperties`、`PropertyExpansion`）
- 设置窗口：`src/app/settings_dialog.cpp`（`ResetSettingsPage`）
- 剪贴板：`src/clipboard/history.h`（`SetLimit`）、`src/clipboard/panel.cpp`（`SetHistoryLimit`、`SetStripStyle`、`SetCardPreview`）、`src/clipboard/stack_items.h`（`Concealed`）

## 测试

- `lumashot_toolbar_test.exe <输出目录>`：布局、命中测试和不重叠检查覆盖 3 种缩放 × 3 种显示器宽度。同时输出预览图：`customized-*`、`pin-more-*`、`pin-context-*`、`labels-text-*`、`labels-icons-*`、`properties-collapsed-*`。
- `ctest -R "settings|preferences|toolbar|clipboard_history|clipboard_entry_flavor"`：设置窗口交互（含两步确认）、按页恢复的范围、IPC 往返与非法值拒绝、INI 读写与样式迁移、历史上限与卡片隐藏。
