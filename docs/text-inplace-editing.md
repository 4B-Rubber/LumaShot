# 文字工具：原位输入

文字和序号说明直接在截图上编辑，所见即所得，不再弹出覆盖内容的输入卡片。

## 交互

- 文字工具单击创建，双击已有文字或序号说明进入编辑，并全选原文字。
- 输入的文字立即用最终样式画在截图上：字体、字号、颜色、粗细、对齐、纯色背景或 Tag 背景都与提交后一致。
- 编辑中显示：
  - 蓝色虚线框（外加一圈白色描边，在深色截图上也看得清）；
  - 按系统闪烁频率闪烁的插入光标，颜色取文字颜色；
  - 选中文字的半透明蓝色底；
  - 输入法预编辑文字的下划线。
- 鼠标：
  - 框内单击移动光标，Shift+单击扩展选区；
  - 框内拖动选择文字；
  - 框内双击选中一段连续的中文或单词；
  - 框外单击完成编辑。这一下只结束编辑，不会马上新建一段文字，跟 Snipaste、微信截图一样。
- 键盘：Enter 完成，Shift+Enter 换行，Esc 取消本次修改，Ctrl+A 全选；方向键、Home/End、复制粘贴和 Ctrl+Z 都沿用系统编辑框的行为。
- 空文字时，编辑框收拢为一个插入点，不会显示一块 300 像素宽的输入区。
- 旋转过的文字按原角度原位编辑，光标和选区跟着一起旋转。
- 不显示提示条。工具栏上的“自定义序号标签”输入框没有可以原位编辑的文字，仍沿用原来的小输入卡片。

## 实现

- `src/app/text_edit.cpp`
  - 原生 EDIT 只负责输入：输入法、光标移动、剪贴板和撤销。
  - 它的宿主窗口放在文字位置，换行宽度与文字相同，样式为 `WS_EX_LAYERED | WS_EX_TRANSPARENT`，透明度为 1/255。窗口完全看不见，鼠标事件直接穿透到画布。
  - EDIT 的每次变更（字符、选区、IME 消息、`EN_CHANGE`）都会调用 `UpdateTextPreview()`：
    - 用 `EditedTextMark()` 生成实时 Mark。提交时也调用同一个函数，所以预览和最终结果逐字节相同。
    - 把 IME 预编辑拼进开始组合时保存的原文快照，避免重复绘制。
    - 记录选区、光标（选区中移动的那一端）和组合范围，写入 `ViewState::text_edit`。
  - 内容没有变化时只重新定位输入法；有变化时重置闪烁并重绘。
  - `PlaceTextIme()` 把系统插入光标（`SetCaretPos`）以及 `ImmSetCompositionWindow`、`ImmSetCandidateWindow` 指向画出来的光标，让候选窗出现在文字旁边。
  - 闪烁由宿主窗口上的 `GetCaretBlinkTime()` 定时器驱动，只重绘画布。
- `src/ui/render.cpp`
  - `Marks` 跳过正在编辑的对象。
  - `Chrome` 在选区裁剪内用 `MarkShape` 画实时 Mark。
  - `TextEditOverlay` 画虚线框、选区、组合下划线和光标，并使用与 `MarkShape` 相同的旋转变换。
- `src/ui/text_bounds.cpp`
  - `MeasureTextEdit` 和 `TextEditIndex` 使用与渲染相同的 DirectWrite 格式和内容框（zh-CN、自动换行、对齐方式）。
  - 用 `HitTestTextPosition`、`HitTestTextRange`、`HitTestPoint` 计算光标、选区和点击位置。
  - 点击位置不会落在 CR/LF 中间。
- `src/app/application.cpp`
  - `PointerDown/Move/Up/DoubleClick` 先交给 `TextEditPointer` 处理。
  - 框内点击时，`WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE`，键盘焦点和正在进行的输入法组合都不会丢。
  - 框内光标显示为 I 形。
- `Renderer::Canvas` 在离屏位图中画出交互画布，供预览和测试使用。

## 验证

- `lumashot_text_edit_test` 与 `--number-label`。
- 预览图：`docs/text-editor-preview.png` 和 `docs/text-editor-dark-preview.png`。
- 测试中间图：`build/text-inplace-*.png`。
