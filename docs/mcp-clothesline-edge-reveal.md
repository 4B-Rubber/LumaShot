# 晾衣绳顶部触发降噪 + 透明区域收起（2026-10-10）

## 问题
钉图打开或已晾起时，光标只要进入屏幕顶部 70 DIP 范围，就会出现蓝色光晕和「晾着 N 张 · 停在顶部展开」标签。
桌面顶部排满应用图标或工具栏时，每次悬停图标都会触发。

## 改动
- `ClotheslineEdge` 新增「显露」状态：光晕和标签只在光标**静止贴住顶边 150 ms**（`kRevealMs`）后出现。
  仅仅靠近顶边、或沿顶边滑动（菜单栏、标签栏），都不再显示。
- 显露后，光标在顶边下方 48 DIP（`kKeepDip`）内保持显示，方便移到标签上；离开这个范围就隐藏，下次需要重新停在顶边。
- 光晕渐隐范围 `kGlowReach` 从 70 DIP 改为 48 DIP，和保持区一致。
- 展开绳子的规则不变：静止 350 ms（最大化窗口下 600 ms）、全屏应用不触发、点击后静默 400 ms、按住鼠标会取消。
- `ClotheslineEdge::Input` 末尾新增 `top`（显示器顶边，物理像素），`Dwelling()` 在标签显示期间也返回 true，以保持 16 ms 轮询。

## 改动二：绳子放下后，停在透明区域也会收起
原来绳子放下后，只要光标在整屏宽、约 270 DIP 高的带状区域里（`ZoneBottom()`），绳子就一直显示，哪怕光标停在透明像素上。
现在由 `Clothesline::KeepOpen()` 判断，只有以下位置能让绳子保持显示（各留 16 DIP 余量，`kKeepMargin`）：
- 顶边到绳子下方一点之间的条带（从顶边进来的路径、挂钩、绳子和上方的「晾着 N 张」条）；
- 卡片本身（`ClotheslineModel::Hit` 新增 `margin` 参数）；
- 操作条，以及空绳时的提示框（新增 `hint_rect_`）；拖动时的预览位。
卡片下方和两侧的透明区域不再算数：光标离开后照旧等 600 ms 收起，卡片之间的间隙可以从容划过。
`ZoneBottom()` 已删除，没有别的地方用到它。

## 涉及文件
- `src/pin/clothesline.h`、`src/pin/clothesline.cpp`、`src/pin/clothesline_model.h`
- `tests/clothesline_test.cpp`：共新增 7 项
  - the rope stays while the cursor is on a card or between the rope and the top edge
  - the transparent space under and beside the cards does not keep the rope open
  - empty rope: the hint keeps it open, the empty space under it does not
  - hovering icons just below the top edge shows no glow or tab
  - sliding along the edge does not reveal the tab
  - a 150 ms rest on the edge reveals glow and tab before the rope
  - the tab stays within reach below the edge and hides once the cursor leaves

## 验证
- `build.bat`：/W4 /WX 编译通过。
- `build\lumashot_clothesline_test.exe`：ALL PASS（含原有全部顶边、钉图用例）。
- 安装包：`dist\LumaShot-Setup.exe`
  - 37,867,050 字节，MZ 头正常
  - SHA-256 `8CFBE6D70D58EAE833D60ED7952495FB893440720F3DEA7DB5B5B7DE673B0946`
  - 包含以上两项改动
  - 包内 LumaShot.exe 与 build 中的一致。未运行安装器，也未替换已安装的应用。

## 可调参数（`clothesline.h`）
`kKeepMargin=16`（`clothesline.cpp`，内容周围仍算「在上面」的余量），`kRevealMs=150`（显露延迟），`kKeepDip=48`（显露后的保持范围），`kDwellMs=350` / `kMaximizedDwellMs=600`（展开绳子所需停留时间）。