# 划词翻译

在任意程序里选中文字，按全局快捷键，鼠标旁弹出翻译气泡。引擎与截图翻译共用（“翻译引擎设置”），只发送选中的文字。

## 入口

| 入口 | 行为 |
| --- | --- |
| 全局快捷键 `Ctrl+Alt+T`（设置 → 快捷键 → 划词翻译，可修改或清除） | 读取当前前台程序中选中的文字并翻译；如果焦点在贴图上，则翻译贴图中选中的 OCR 文字 |
| 贴图右键菜单“翻译选中文字” | 翻译贴图中选中的 OCR 文字（没有选择时灰显） |

快捷键属于主机注册的全局热键（RegisterHotKey id 7），与其他快捷键一起受“禁用快捷键 / 全屏游戏时禁用”控制。升级用户若已有快捷键占用 `Ctrl+Alt+T`，新默认值自动留空，不会抢占。

## 读取选中文字

1. **贴图优先：** 前台窗口是贴图（或其选字工具条）时直接使用 `ocr::Selected` 的结果；没有选择时气泡提示先拖选文字。
2. **UI Automation：** 在工作线程（MTA）里取焦点元素的 `TextPattern::GetSelection`，最多等待 400 ms；目标程序无响应时直接放弃这个结果，工作线程分离后自行结束。浏览器、Word、记事本、终端等大多数程序都在这一步完成，**不碰剪贴板**。密码框直接放弃，也不会进入下一步。
3. **复制兜底：** 不支持 TextPattern 的程序改用模拟 `Ctrl+C`：
   - 先等热键的修饰键松开（最多 700 ms），仍按住时注入一个未分配按键（0xE8）再抬起修饰键，避免 Alt/Win 单击打开菜单；
   - 复制前保存剪贴板中所有全局内存格式（跳过 GDI 句柄类格式，系统会从 DIB / Unicode 文本重新合成），总量上限 256 MB；
   - 等待剪贴板序号变化（最多 600 ms，期间只处理跨线程发来的消息，保证 WM_RENDERFORMAT / WM_DESTROYCLIPBOARD 能被应答），读取 `CF_UNICODETEXT`；
   - 立即恢复原剪贴板，并附带 `ExcludeClipboardContentFromMonitorProcessing`，LumaShot 自己的剪贴板历史和其他历史工具都不会记录这次恢复；复制和恢复都在 UI 线程上完成，期间不处理剪贴板通知，因此 LumaShot 历史里也不会出现那段临时文字。Windows 自带的 `Win+V` 历史仍可能记下这次复制。
   - 终端类窗口（conhost、Windows Terminal、mintty、PuTTY、WezTerm、Alacritty 等）不发送 `Ctrl+C`，以免没有选择时变成中断信号。
4. 文本规整：统一换行为 LF、去掉控制字符和首尾空白，最多 5000 字。空行分段、列表项（`- `、`• `、`1. `、`2) `、`3、`）单独成段，最多 64 段，作为一次翻译任务的多个条目发送。

## 气泡

翻译面板的文字模式（`TranslationView::text_mode`），与贴图翻译面板同款外观，全部文字经 TextRenderer 绘制：

- 标题“划词翻译”，副标题为引擎与“源语言 → 目标语言”；
- “固定”开关代替贴图面板的“译文 / 原图”分段；语言芯片可临时改译其他语言（本次会话内记住）；
- 列表逐段显示原文与译文，点一段复制该段；底部“复制译文”“复制双语”；
- 未配置引擎时显示“打开翻译设置”，保存设置后自动重试；失败时显示原因与“重试”；
- 高度随内容变化（250–560 DIP），默认出现在光标右下方，靠近屏幕边缘时翻到左侧 / 上方，始终在工作区内；
- 气泡 `WS_EX_NOACTIVATE`，不抢焦点，原程序的选择和输入不受影响。

关闭规则：未固定时，`Esc` 或在气泡外按下任意鼠标键即关闭（只在气泡可见时以 40 ms 轮询 `GetAsyncKeyState`，不安装钩子、不吞掉 Esc）；固定后只能点 × 关闭，可拖动标题栏空白处移动，再次划词会在原位置更新内容。

## 代码

- `src/pin/selection_capture.*`：规整、分段、剪贴板快照、UI Automation 与复制兜底；
- `src/pin/selection_translator.*`：气泡生命周期、翻译任务（`translate::Service`）与关闭规则；
- `src/pin/translation_panel.*`：文字模式、`PlaceNear`、标题栏拖动；
- `src/app/application.cpp`：热键 id 7 → `Application::TranslateSelection`；
- `src/app/hotkeys.h`、`preferences.*`、`settings_process.cpp`（IPC 版本 16）、`settings_dialog.cpp`：快捷键设置。

测试：`lumashot_selection_translate_test`（规整 / 分段、默认快捷键与冲突让位、气泡位置与高度、未配置引擎时的状态，不访问真实剪贴板和前台程序），以及更新后的 `lumashot_settings_test`、`lumashot_hotkey_policy_test`、`lumashot_pin_menu_test`。
