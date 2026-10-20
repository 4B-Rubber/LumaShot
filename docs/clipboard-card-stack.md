# LumaShot UI 重构方案：full.html 面板 + 卡片堆叠贴边

> 版本：2026-10-17 v1.1（加入性能预算）· 状态：已确认方向，待 Spike 验证（尚未改动应用代码）
> 依据：仓库根目录 `full.html`（完整重构原型）、`lumashot_redesign/strip-concepts.html` 的 **D 卡片堆叠**、以及只读查阅的源码：
> `src/clipboard/{panel.cpp,panel.h,layout.h,history.h,liquid_surface.*,liquid_motion.h,strip_dismiss.h,quick_window.h,preview_cache.h,syntax_highlight.h,native.h}`、
> `src/ui/{acrylic.*,glass_surface.h,window_surface.h,toolbar.cpp}`、`src/pin/{pin.h,pin.cpp,clothesline.*}`、`src/export/drag_out.*`、`src/app/application.*`、`docs/*`。

---

## 0. 一句话

**贴边 = 最近剪贴板的实体卡片**：平时露出最近 3 张，悬停扇开成 5 行，点击就粘贴；**每张卡片都能拖**：拖到程序里就插入，拖到桌面就钉图，拖回堆叠就置顶。
展开后是 full.html 的分类卡片面板。贴边、面板、钉图、截图共用一套材质和一条"物件流转"路径，解决三个痛点：

| 痛点 | 根因（代码） | 方案中的解决方式 |
|---|---|---|
| 折叠条展开后才变亚克力，割裂 | 折叠条是自绘实色液态形状（`liquid_surface.cpp` 填 `0xf5f8fc / 0x192330`），展开面板是另一个 HWND，用 DWM Acrylic（`DWMSBT_TRANSIENTWINDOW`，`PanelOpacity` .60/.72） | 堆叠的每张卡片和把手都是 **独立的亚克力玻璃**（合成层 HostBackdrop，§3.3），与面板的系统亚克力按 Token 对齐；只在拖动时临时变实色，松手 120 ms 后淡回 |
| 工具面板、剪贴板、钉图互不相干 | `Application` 同时持有 `clipboard_panel_` 和 `pins_`，两者之间 **没有任何回调**（`application.cpp:141-182`） | 新增 4 条连线：截图飞入堆叠、卡片 → 钉图、钉图 → 堆叠、面板图片 → OCR / 标注后钉图 |
| 剪贴板太素 | 条目只按 `Kind{Text,Image,Files}` 画 | 引入展示用的 `Flavor`（代码 / 链接 / 颜色 / 路径），按类型渲染卡片和动作，与 full.html 一致 |

---

## 1. 范围与验收

### 1.1 范围
- **改**：剪贴板折叠形态（换成卡片堆叠）、展开面板视觉和交互、条目拖出、钉图悬停栏、模块联动、统一设计 Token。
- **保留不动**：截图选区 / 标注引擎、统一工具栏（已在 `toolbar.cpp` 实现属性抽屉，只对齐 Token）、晾衣绳逻辑、会话存储与加密、Win+V 接管策略、连续粘贴（固定面板）、快速窗口 `QuickWindow`、拖到底部关闭目标隐藏侧边条（`DismissTracker`）。
- **不照搬 full.html 的部分**：左上角启动坞和欢迎条（原型演示用；真实入口是托盘菜单和热键），以及"Ctrl+V 粘贴进原型"。

### 1.2 验收标准（贯穿各阶段）
1. 折叠、扇开、展开三种形态 **材质一致**：浅色和深色、100/150/200% DPI 都截图对比，四角无黑边。
2. **所有类型的卡片都能拖出**（文本 / 代码 / 链接 / 颜色 / 文件 / 图片），在堆叠和面板里都能拖；拖动阈值使用 `SM_CXDRAG/SM_CYDRAG`。
3. 静止时 **没有常驻计时器、轮询或渲染循环**：动画只用合成动画（Windows.UI.Composition / DComp，在 DWM 中运行）或"动画期间才存在"的计时器；悬停检测用 `WM_MOUSEMOVE` + `TrackMouseEvent(TME_LEAVE)`。
4. **性能达标**：满足 §1.3 的全部预算。任何一项超标，该阶段就不算完成。
5. 所有界面文字走 `TextRenderer` / LumaText；C++20、/W4 /WX 编译通过。
6. 测试只用合成数据，不读个人文件或剪贴板内容；临时产物放在 `build/`。
7. 每个阶段都构建 `dist/LumaShot-Setup.exe` 并报告 SHA-256，不运行安装包。

### 1.3 性能预算（硬约束：快、占用低）

基线在 S0 用现在的经典细条实测（同一台机器、同一套合成数据），之后每个阶段的报告都附上与基线的对比表。已有文档中的参考值：折叠态私有提交约 106–109 MiB（合成测试进程）；折叠后重新打开约 65–69 ms（含测试夹具稳定时间）；连按空格重新打开预览的中位数约 16 ms。

| 类别 | 指标 | 预算 | 测法 |
|---|---|---|---|
| **空闲** | 折叠态静置 60 s 的进程 CPU 时间增量 | ≤ 5 ms | `GetProcessTimes` 前后差 |
| | 静置期间的计时器和 UI 线程唤醒 | 0 个常驻计时器；除系统广播外 0 条消息 | 测试构建中的计时器 / 消息计数钩子 |
| | GPU 模糊区域 | 折叠时只有把手（30×22 DIP）；卡片不挂背景画刷 | 视觉树审计 |
| **内存** | 折叠态私有提交 | ≤ 基线 + 2 MiB | `docs/clipboard-folded-memory.md` 的方法 |
| | 扇开时额外占用 | ≤ 4 MiB；收起后 10 s 内回落到折叠预算内 | 同上 |
| | 展开面板（100 条合成数据） | ≤ 基线 | `clipboard_resource_audit_test` |
| | 冷启动 | 时间和内存都不高于基线 | `cold_start_memory_test`、启动计时 |
| **速度** | 悬停停留 120 ms 后 → 扇开动画提交 | ≤ 16 ms（一次提交，后续由 DWM 运行） | QPC 埋点 |
| | Win+V / 点把手 → 面板首帧 | ≤ 基线，目标比基线快 20% | 现有重新打开测法 |
| | 单击卡片 → 发出粘贴 | ≤ 30 ms（不含目标程序处理时间） | QPC 埋点 |
| | 复制完成（`WM_CLIPBOARDUPDATE`）→ Incoming 开始 | ≤ 50 ms；缩略图没准备好时先显示类型占位，准备好后替换 | QPC 埋点 |
| | 越过拖动阈值 → 拖影出现 | ≤ 1 帧；数据格式在目标真正请求时才生成 | QPC 埋点 |
| | 动画期间应用侧 CPU | ≈ 0；UI 线程没有逐帧工作 | ETW / `GetThreadTimes` |

### 1.4 性能设计原则

1. **动画交给系统合成器**：Windows.UI.Composition 是 Windows 内置的系统合成器，不需要部署任何运行时；动画在 DWM 进程中运行，应用线程不需要逐帧工作。窗口尺寸只在动画开始和结束时各改一次。
2. **不新建第二套图形设备**：卡片表面的 `CompositionGraphicsDevice` 使用现有的"每个 UI 线程共享一个 D3D11 设备"缓存（`docs/clipboard-shared-device-memory.md`）。合成器每个进程只有一个，按需创建。
3. **只为看得见的东西付费**：
   - 折叠时卡片的玻璃画刷摘掉（`Brush = null`），只保留把手的玻璃。
   - 面板列表只渲染可见的卡片。
   - 缩略图只解码前 5 条，最长边 96 px，在工作线程解码，统一归入 `PreviewCache` 的预算。
4. **算一次，缓存下来**：Flavor 在入库时计算（手写判断，不用正则库）；文字排版沿用 LumaText 的现有缓存；卡片内容表面只在条目变化时重画。
5. **惰性创建**：合成器和卡片表面在程序启动后第一个空闲消息时才创建，不占冷启动的关键路径；首帧之前只创建窗口。拖动的数据格式在 `GetData` 时才生成，文本类直接引用 `Entry::formats`，不复制。
6. **事件驱动，不轮询**：悬停用 `TrackMouseEvent`；所有状态切换都由消息或合成批次的完成事件驱动。
7. **空闲回收**：10 s 时调用 Trim（现有逻辑）；60 s 时释放对绘制设备的引用（合成表面保留，外观不变）；超出前 5 条的缩略图交给 `PreviewCache` 按 LRU 淘汰。
8. **可度量**：在 `clipboard_perf_test`、`idle_memory_test`、`cold_start_memory_test`、`clipboard_resource_audit_test` 中加入上表的断言；超过预算时测试直接失败。

---

## 2. 设计 Token（Luma Glass）

从 full.html 的 CSS 和现有代码常量合并而来，放在 `src/ui/luma_glass.h`（只放 constexpr 常量，无逻辑；S0 已新增）。`PanelBackground` / `PanelOpacity`（`acrylic.h`）、`ToolPanelRadius`（`glass_surface.h`）和折叠条液态填充色（`liquid_surface.cpp`）已改为引用这些常量，数值不变。其余重复写死的颜色（`panel.cpp`、`settings_dialog.cpp`、`preview_window.cpp`、`quick_window.cpp`、`canvas.cpp`、`pin_menu.cpp`、`worker.cpp`、`themed_message.h`）在各自阶段改动对应文件时再迁移，避免与未提交的功能改动冲突。

| Token | 浅色 | 深色 | 来源 / 说明 |
|---|---|---|---|
| `Glass.Fallback`（无亚克力时的实色） | `#F7F9FC` | `#252C38` | = `PanelBackground` |
| `Glass.Opacity` | .60 | .72 | = `PanelOpacity` |
| `Glass.Border` | `rgba(255,255,255,.75)` | `rgba(141,167,197,.22)` | full.html `--glass-border`，用于 `DWMWA_BORDER_COLOR` 和内描边 |
| `Card.Fill` / `Card.Hover` | `rgba(255,255,255,.58)` / `.88` | `rgba(255,255,255,.055)` / `.11` | full.html `--card` |
| `Ink.1/2/3` | `#18212D / #566174 / #8A94A3` | `#E9EEF5 / #AAB5C4 / #7C8898` | full.html |
| `Accent` | `#1A7CFF` | 同 | 品牌主色 |
| `Radius.Panel / Card / Control` | 14 / 10 / 8 DIP | 同 | `ToolPanelRadius=14` 不变 |
| `Shadow.Card` | y 6, blur 18, 16% | 35% | 卡片 / 拖影 |
| `Motion.Spring` | 频率 29、阻尼 .82 | — | 沿用 `LiquidSpring` 默认值 |
| `Motion.Fast / Base / Settle` | 120 / 180 / 320 ms | — | 淡入淡出 / 扇开 / 吸附回弹 |

系统关闭动画时（现有 `MotionEnabled()` 为 false），所有过渡直接跳到终态。

---

## 3. 贴边卡片堆叠（核心）

### 3.1 形态与尺寸（DIP，右侧贴边为例，左侧镜像）

```
 折叠（Rest）                      扇开（Fan）
 ┌────┐ ← 把手 30×22，显示总数       ┌──────────── 把手 ─┐
 │ 12 │   亚克力                    │ 12   全部 ›       │
 └────┘                            ├───────────────────┤
   ▟▀▀▀▀▀▀│   卡片 0  52×40，-2°     │▣ 截图 520×300   1 │ 行 208×46，间距 50
   ▟▀▀▀▀▀▀│   卡片 1  缩放 .94，+2°  │▣ const create…  2 │
   ▟▀▀▀▀▀▀│   卡片 2  缩放 .88，+6°  │▣ Acrylic mat…   3 │
        露出 38 DIP，其余在屏幕外     │▣ #1A7CFF        4 │
                                   │▣ 需求文档.pdf    5 │
                                   └ 还有 7 条 · 点击把手 ┘
```

> S1 起以 `strip-concepts.html` D 标签页的 CSS 为准（逐值写入 `src/clipboard/stack_layout.h`，由 `clipboard_stack_layout_test` 校验）。下表坐标以 HTML 的 `.D` 盒子（230×220，贴右边）为原点。

| 参数 | 值（DIP） | 来源 / 备注 |
|---|---|---|
| 迷你卡片 | 52×40，圆角 7；`right:-14`，`top 70+9i`；`translateX(7i) rotate((2−4i)°) scale(1−.06i)`，中心为原点；只显示 i<3 | `.D .sc` + `layoutD()`；超出屏幕的 14 DIP 由窗口边缘（即屏幕边缘）裁掉 |
| 迷你卡片外观 | 缩略图铺满；`0 6px 16px rgba(0,0,0,.25)` + 1.5 px 白色外环（.9） | 玻璃不透明度为 0 |
| 扇开行 | 208×46，圆角 10，内边距 3；`right:10`，`top 30+50i`，最多 5 行；悬停向内移 4 | 玻璃 + `--shadow`（`0 18px 48px` + `0 2px 6px`） |
| 行内容 | 缩略图 46×40 圆角 7；文字从 x=57 起：标题 12 px 粗体 ink，副标题 10.5 px ink3；右侧 `Alt+N` 徽标 10 px 粗体、1 px edge 描边、圆角 4 | 缩略图尺寸与圆角**瞬时**切换（HTML 中 `.thumb` 没有过渡），位置随内边距动画移动 |
| 把手 | 30×22，贴边一侧不留圆角（内侧圆角 8），玻璃 + 1 px gborder；数字 11 px 700 accent；`top 42`（折叠）/ `2`（扇开），.3 s ease | 暂停时数字改为 ink3，并在前面画两条暂停竖线 |
| "还有 N 条 · 滚轮翻看 · 点数字打开" / 翻看时 "第 a–b 条，共 N 条" | `top 282`，`right 14`，白色 11 px，`text-shadow 0 1px 2px #000a` | 总数大于 5 或正在翻看时显示（见 3.8） |
| 动画 | 卡片 .32 s `cubic-bezier(.3,1.25,.5,1)`；新卡片从 `translateX(80) rotate(8°)`、透明开始，.5 s `cubic-bezier(.3,1.3,.5,1)` | 左侧镜像（位移和角度取反） |
| 窗口 | 固定 286×378：盒子 230 + 内侧 56；上 8 + 内容 298 + 下 72。贴边一侧不留边距 | 边距只为容纳阴影；窗口大小在折叠和扇开之间**不变**，不需要动画前后调整窗口 |
| 定位 | 把手顶边 = 经典细条保存的锚点顶边；扇开后的内容整体限制在工作区内；贴近哪条边就贴哪侧 | `stack::WindowRect` |

### 3.2 状态机

```
            悬停 120ms              离开 300ms 宽限
   ┌──────┐ ─────────────▶ ┌─────┐ ─────────────▶ Rest
   │ Rest │                │ Fan │
   └──┬───┘ ◀───────────── └──┬──┘
      │  新条目：Incoming（新卡片从屏外滑入，压住旧卡片，320ms）→ 回到原状态
      │  无操作 60s：Idle（释放绘制设备，合成表面保留，外观不变）
      │  拖把手：MoveStack（实色）→ 吸附 → 120ms 淡回亚克力
      │  拖卡片：DragCard（OLE 拖放，见 3.5）
      │  点把手 / Win+V：Panel（卡片飞到列表对应位置，面板展开）
      └  拖到底部关闭目标：Hidden（沿用 DismissTracker）
```

- **Incoming**：只在新条目进入历史时播放一次（`WM_CLIPBOARDUPDATE` → `Read()` → `History::Add` 成功）。重复内容被置顶时，只播放轻微的"抽出再插回"。
- **Paused**：沿用 full.html 的"记录中 / 已暂停"。暂停后卡片降低饱和度，把手显示暂停徽标；复制时不播放 Incoming。
- **隐私**：新增设置项"侧边条显示内容预览"（默认开）。关闭后卡片只显示类型图标和序号，扇开行只显示类型和时间。

### 3.3 Win32 架构：逐卡亚克力（与 HTML 效果一致）

#### 为什么不用"一张底板"
先比较三种做法能否还原 HTML 的效果。HTML 的扇开态有四个特征：**每行都是独立的玻璃卡片，行间 4 DIP 的空隙能透出桌面**；圆角为 10；卡片从旋转的堆叠位姿边移动边转正，玻璃同时淡入；悬停的那一行单独左移 4 DIP。

| 做法 | 独立玻璃 + 行间空隙 | 圆角 10 | 旋转中带玻璃 / 玻璃淡入 | 单行悬停位移 | 结论 |
|---|---|---|---|---|---|
| A. 一张 DWM 亚克力底板放 5 行（原推荐） | ✗ 只有一整块玻璃 | ✗ 只能用 DWM 固定圆角 | ✗ | 只能移动内容，不能移动玻璃 | **达不到，否决** |
| B. 每行一个 DWM 亚克力窗口（5 个 HWND） | ✓ | ✗ DWM 圆角固定为 8，不能自定义 | ✗ 窗口不能旋转；系统背景窗口无法平滑淡入，只能显示或隐藏 | ✓（`SetWindowPos`） | 接近，但过渡动画达不到 |
| **C. 合成层逐卡亚克力（新推荐）** | ✓ 每张卡片单独采样桌面 | ✓ 用圆角裁剪实现任意半径 | ✓ 旋转、缩放、透明度都可以做合成动画 | ✓ | **可以达到 HTML 效果** |

#### 做法 C 的实现（S1 已实现，2026-10-17）
Spike 和两轮补充探针（`build/spike/probe.cpp`、`probe2.cpp`）改变了几处原设想，下面是最终做法：

- **两个窗口，同一矩形**（探针 1 的结论）：
  - `SetWindowRgn` 会裁掉合成内容，阴影会被切断，所以**画面窗口不设区域**。它的样式是 `WS_EX_NOREDIRECTIONBITMAP | LAYERED | TRANSPARENT | TOOLWINDOW | TOPMOST | NOACTIVATE`，加 `LWA_ALPHA 255`，完全点击穿透。实测 HostBackdrop 在这种分层窗口里正常工作。
  - **输入窗口**是一个没有任何内容的 NOREDIRECTIONBITMAP 窗口（DWM 属性 34=无边框色、33=不圆角、关闭过渡动画，实测不可见）。它由画面窗口拥有，所以总在其上方。它的区域 = 卡片形状的并集：折叠时是可见迷你卡片（按旋转后四角）+ 把手；扇开时是 HTML 盒子 + 各行 + 把手（对应 `.D.open` 的 `pointer-events:auto`）。`HTTRANSPARENT` 只能在同线程窗口间转发，不能用。
- **材质**：每张卡片 = HostBackdrop 画刷（DWM 已做模糊）+ `CardGlass` 着色层，不需要自己实现高斯模糊效果。着色 Token 来自 Spike 的 ΔE 标定：浅色 `#f7f9fc` @ .84（ΔE 0.93），深色 `#323a48` @ .72（平均 3.06，最差约 4.2，已确认接受 ≤ 4.5）。
- **视觉树**（每张卡片按条目 id 复用）：
  ```
  root ContainerVisual（Offset / Size / CenterPoint / Rotation / Scale / Opacity 动画）
   ├─ 远阴影 SpriteVisual + DropShadow（圆角九宫格遮罩；Blur/Offset/Color 在迷你阴影与 --shadow 之间动画）
   ├─ 近阴影（0 2px 6px，扇开时淡入）
   ├─ body（圆角 GeometricClip，半径 7→10 动画）
   │   ├─ HostBackdrop + 着色（Opacity 0→1，对应 HTML 背景从透明过渡到玻璃）
   │   ├─ 缩略图（UniformToFill，自身圆角裁剪）
   │   └─ 文字表面（扇开时瞬时出现）
   └─ 白色外环九宫格（扇开时淡出）
  ```
- **绘制设备：WARP**（探针 2 的结论）：
  - 硬件 D3D11 设备 + D2D + 合成图形设备会增加 41 MiB，而且无法释放（`SetRenderingDevice(nullptr)` 返回 `E_INVALIDARG`）。
  - WARP 设备 Trim 后只占 +2.9 MiB。缩略图和文字只在历史变化时画一次，合成本身仍由 DWM 在 GPU 上完成。
  - 每批绘制后 250 ms 用一次性计时器调用 `ClearResources` + `ClearState` + `Flush` + `Trim`。不能更早：WARP 在提交时才执行绘制，会产生约 +5 MiB 的瞬时占用，Trim 后全部归还。
- **动画**：全部是合成关键帧动画，由 DWM 运行；应用侧只有 120 ms 悬停、300 ms 离开、250 ms Trim 三个**一次性**计时器，空闲时没有计时器。
- **线程**：`BackdropCompositor` 每个 UI 线程一个（DispatcherQueue 依附在现有消息循环上），用 WRL 调用 ABI 接口，不引入 C++/WinRT 或 WinAppSDK。
- **退路**（Windows 10、22000 以下版本、关闭透明效果、高对比度、合成调用失败时）：着色层改为不透明，HostBackdrop 隐藏，形状、动画和交互不变。`Diagnostics.last_error` 会记录失败的 HRESULT 和行号。
- **DPI / 多屏**：坐标用物理像素，UI 尺寸用 DIP；缩放变化时复用同一个 `DesktopWindowTarget`（一个窗口只能有一个），重建视觉树和表面。
- **位置保存**：沿用 `layout.h` 的 `SavePosition`（`[Panel] Right/Top`），新增 `[Strip] Style=Stack` 或 `Classic`。

#### S1 实测（Windows 11 22631，150%，`clipboard_stack_layout_test`）
| 项目 | 结果 | 预算 |
|---|---|---|
| 扇开 / 收起（UI 线程耗时） | 2.05 ms / 1.43 ms | ≤ 16 ms |
| 静止时计时器 | 0 | 0 |
| 单独运行时的私有内存：折叠 / 扇开 | +4.3 MiB / +4.4 MiB | 折叠 ≤ +2，扇开 ≤ +4（见下） |
| 合成错误 | 无，玻璃开启 | — |

内存说明：底线是 WARP 设备（+2.9 MiB，只要还有绘制表面就不能释放），再加上合成器（+0.3）和视觉树。这个数字是**相对没有任何折叠条**的进程测的。原预算是相对经典细条定的：经典细条折叠可见时整个进程约 59.6 MiB（基线），卡片堆叠不再需要它的硬件合成设备。所以净效果要在 S3 接入面板后按基线方法复测；如果仍超出预算，就带数据回来确认。

> **Spike（S1 前两天，结论决定能否进入 S1）**：用最小程序验证以下 5 项，并在 `build/` 中留下截图和数据。
> 1. 一个窗口内 6 个 HostBackdrop 卡片同时显示，100/150/200% DPI 下都正确，空隙透出桌面。
> 2. 输入区域（`SetWindowRgn`）与 HostBackdrop 同时使用时，画面不被裁坏，空隙可以点击穿透到下方程序。
> 3. 旋转 + 位移 + 玻璃淡入的合成动画在 60 Hz 和 144 Hz 显示器上都流畅，应用侧 CPU 接近 0。
> 4. 与面板系统亚克力的色差 ΔE ≤ 3（浅色、深色各一组合成背景）。
> 5. 性能：引入合成器后，折叠态私有提交 ≤ 基线 + 2 MiB；扇开时额外占用 ≤ 4 MiB；空闲时 CPU 和唤醒都为 0；动画期间应用侧 CPU ≈ 0。同时测一组"不带玻璃"的对照，量化玻璃效果本身的开销。
>
> 任何一项不通过，就带着数据回来和你确认（例如：第 2 项不通过，改用"透明空隙也算命中、离开卡片 300 ms 后自动收起"的方案；第 5 项不通过，调整预算）。**不会悄悄降级到做法 A。**

### 3.4 卡片内容

| Flavor | 迷你卡片（52×40） | 扇开行 / 面板卡片 | 数据来源 |
|---|---|---|---|
| 图片 | 缩略图（cover 裁切） | 缩略图 + `宽×高` + "含 N 行文字" | `native::Thumbnail(entry, 96)` → `PreviewCache`（8 MiB 上限） |
| 代码 | 深色底 + `</>` + 语言 | 前 2 行高亮代码 | `HighlightCode(text, Auto)`（已有异步高亮） |
| 链接 | 域名首字母头像（按域名哈希取色） | 标题 / 域名 | 文本解析，不联网 |
| 颜色 | 纯色块 | 色块 + HEX + rgb() | 文本解析 |
| 文件 | 扩展名徽标 | 文件名 + 路径 + 数量 | `file_names[3]`、`file_count`、`file_icons.cpp` |
| 文本 | `Aa` + 首字 | 前 2 行 | `Entry::text` |

`Flavor` 只在入库时计算一次，缓存在 `Entry` 中，**不写入会话存储**（加载时重新计算，避免会话格式升版本）：

```cpp
// src/clipboard/entry_flavor.h（纯函数，可单测）
enum class Flavor : uint8_t { Plain, Code, Link, Color, Path, Image };
Flavor DetectFlavor(const Entry& e);   // Kind::Image → Image；Kind::Files → Path；
                                        // Text：单行 URL → Link；#RRGGBB / rgb() → Color；
                                        // HighlightCode 的结果非 Plain 且多行 → Code；否则 Plain。
                                        // 只检查前 4 KiB。
```

### 3.5 拖动规则（每张卡片都能拖）

以现有的 `src/export/drag_out.{h,cpp}` 为基础。钉图已经用它的 `DragImageOut` 拖出图片（OLE `DoDragDrop`，提供 CF_HDROP / CF_DIB / PNG，在工作线程编码）。

| 拖动来源 | 松手位置 | 结果 | 实现 |
|---|---|---|---|
| 任意卡片（堆叠或面板） | 接受拖放的程序 | **插入**内容 | `DoDragDrop`，按类型提供格式（见下表） |
| 图片卡片 | 桌面 | **钉图**（在松手位置生成钉图） | `QueryContinueDrag` 松手时发现指针在桌面，返回 `DRAGDROP_S_CANCEL` → 调用 `PinRequest` |
| 图片卡片 + 按住 Alt | 桌面 | 存为 PNG 文件 | 不拦截，交给资源管理器接收 CF_HDROP |
| 文本 / 代码 / 链接卡片 | 桌面 | 存为 `.txt` / `.url` | 虚拟文件格式 `CFSTR_FILEDESCRIPTORW` / `CFSTR_FILECONTENTS`，由资源管理器生成文件 |
| 任意卡片 | 回到堆叠 / 面板 | **置顶**（相当于 Alt+1 的位置） | LumaShot 不注册拖放目标，结果为 NoTarget；再按释放点判断是否落在堆叠区域 |
| 任意卡片 | 不接受拖放的程序（如 UIPI 提权窗口） | 不生成钉图；提示"目标不接受拖放，已复制，可按 Ctrl+V" | NoTarget 且不在桌面 → 写入剪贴板并提示 |
| 钉图 | 堆叠 | **加入历史** | 见 §5.3 |
| 把手 | 任意位置 | 移动堆叠 / 换边 / 拖到关闭目标隐藏 | 现有 `SnapAxis` + `DismissTracker` |
| 卡片单击 | — | 粘贴到之前的前台窗口 | 现有粘贴管线（`RememberTarget`、焦点和进程校验） |
| 面板多选后拖动 | 程序 | 文本类合并后插入；文件类合并成多文件 CF_HDROP；图片暂只支持单张 | 与 full.html 的"合并复制"一致 |

各类型提供的格式（全部来自 `Entry::formats`，不重新编码文本）：

| Flavor | 格式 |
|---|---|
| Plain / Code | `CF_UNICODETEXT`，以及原条目保存的 `HTML Format` 和 `Rich Text Format` |
| Link | 上述格式 + `UniformResourceLocatorW` |
| Color | `CF_UNICODETEXT` |
| Path | `CF_HDROP`（只包含仍然存在的路径；全部失效时禁止拖动并提示"文件已不存在"） + `CF_UNICODETEXT`（路径） |
| Image | 现有：`CF_HDROP`（临时 PNG，由 24 h 清理删除）+ `CF_DIB` + `PNG` |

`drag_out.h` 的扩展（原有 `DragImageOut` 改为对新接口的薄封装，钉图调用方不用改）：

```cpp
struct DragPayload {
    std::vector<clipboard::Format> formats;                  // 文本类：原样提供
    std::vector<std::filesystem::path> files;                // Path：CF_HDROP
    std::function<std::shared_ptr<const Frame>()> image;     // Image：工作线程编码
    std::wstring virtual_file_name;                          // 拖到桌面时生成的 .txt / .url
};
enum class ReleaseDecision { Default, Intercept };           // Intercept → DRAGDROP_S_CANCEL
struct DragOutOptions {                                      // 在原有字段上增加：
    std::function<ReleaseDecision(POINT cursor, DWORD keys)> on_release;
};
DragOutResult DragOut(DragPayload payload, DragOutOptions options);
```

判断桌面：用 `WindowFromPoint` 取得窗口，用 `GetAncestor(GA_ROOT)` 取根窗口，检查其类名是否为 `Progman` 或 `WorkerW`，且下面有 `SHELLDLL_DefView` 子窗口。判断失败时视为普通程序（退化为存成文件，不会误生成钉图）。

拖动中的视觉：
- 源卡片变成半透明（25%）的占位。
- 拖影使用 `IDragSourceHelper::InitializeFromBitmap`，图像为渲染好的行卡片（208×46，−3°）。
- 在桌面上显示"钉图"图标；靠近堆叠时拖影缩小到 .92 并显示插入空位。

#### S2 第 1 部分实现记录（2026-10-17）

已实现：堆叠卡片整卡拖出。代码在 `src/clipboard/card_drag.{h,cpp}`，面板接入在 `ClipboardPanel::Impl::StackDrag`，测试为 `clipboard_card_drag`（47 项）和 `clipboard_stack_panel`（新增 14 项）。与上面的计划有以下出入：

- **独立模块，不扩展 `drag_out`**：`drag_out.cpp` 属于 `lumashot_pin`，面板所在的 `lumashot_ui` 不链接它。新模块照搬它的模式（工作线程准备 + `shared_future`、`extra_` 存放 shell 写入的数据、`SHCreateStdEnumFmtEtc`），钉图代码不受影响。
- **不用虚拟文件**：如果提供 `FILEDESCRIPTOR`，聊天软件和浏览器可能会接收"文件"而不是文字。所以文本、代码、链接卡片在桌面上松手时由 `on_release` 拦截，LumaShot 自己往桌面写 `.txt`（UTF-8 带 BOM，全文）或 `.url`；文件名取首行，去掉非法字符，同名时加 " (2)"，永不覆盖。资源管理器的文件夹窗口不接受文字，结果是 NoTarget，改为复制到剪贴板并提示。
- **格式读取在 `DoDragDrop` 之前完成**：`GetData` 在 UI 线程（STA）上被调用，不能等待需要 `DrainStore` 才能送达的读取结果，否则会死锁。最多等 800 ms，超时就放弃本次拖动并提示。
- **文件卡片**：原样提供保存的 `CF_HDROP`。只有全部路径都失效时才拒绝拖动并提示"文件已不存在"，部分失效的路径不做过滤。
- **图片卡片**：拖动开始时在工作线程把图片写成 `%TEMP%\LumaShot-Clipboard\LumaShot-*.png`（已保存的 PNG 原样写出，否则把 DIB 转成 PNG），由 24 h 清理删除。钉图需要应用层注册 `ClipboardPanel::SetPinHandler`；没有注册时（或按住 Alt）不拦截，由资源管理器把 PNG 存到桌面。
- **拖动视觉**：源卡片变成 25% 的占位；拖影是用 CPU（WIC）渲染的行卡片，208×46，−3°，带阴影，与堆叠使用同一套绘制代码。"钉图"图标和靠近堆叠时缩小到 .92 放到第 2 部分。
- **提示**：目前只写入面板状态栏，堆叠本身还没有提示气泡（第 2 部分或 S5）。

#### S2 第 2 部分实现记录（2026-10-17）

- **自绘拖影**：系统的 `IDragSourceHelper` 不能在拖动途中改变图像，所以改用两个只在拖动期间存在的透明分层窗口（不参与命中测试，不抢焦点），照 strip-concepts.html 概念 D 绘制：
  - **卡片**：抓在 (26, 22) 处，默认 rotate(−3°) scale(1.03)；靠近来源（堆叠或展开的面板）时 rotate(0) scale(.92)、不透明度 .8；在桌面上加 2 px 强调色描边。
  - **动画**：变换过渡 .15 s ease，只在过渡期间开 15 ms 计时器。
  - **指针提示**（`.cursorhint`）：11 px 白字、rgba(10,20,40,.6) 底、圆角 6，位于指针 (+16, +18)，靠近屏幕边缘时翻到左侧，首次出现时 .2 s 淡入。
  - **提示文字**：
    - 放回并置顶
    - 插入到「窗口标题」，标题用 `InternalGetWindowText` 读取，挂起的程序也不会卡住拖动
    - 钉到桌面（按住 Alt 存为文件）
    - 在桌面存为 .png / .txt / .url
    - 复制到桌面
    - 拖到程序或桌面
- **面板卡片整卡可拖**：展开的面板中，按住卡片移动超过系统拖动阈值就开始拖动，被拖的卡片变成 25% 透明（`.lifted`）；拖回面板就置顶。按卡片时不捕获鼠标：释放捕获会触发 `WM_CAPTURECHANGED`，从而取消固定面板"单击粘贴"的待处理点击（`clipboard_continuous_paste` 测出了这个问题，已修复）。
- **钉图已接入**：`application.cpp` 调用 `SetPinHandler`，以松手点为中心调用 `pins_->Create`（不自动 OCR）。
- **与 HTML 的差异**：拖影背景用实色 `--solid`（.97 不透明），没有毛玻璃，因为分层窗口无法低成本地实时模糊背后的内容。拖影按堆叠所在显示器的 DPI 绘制；经典样式没有堆叠渲染器，因此不显示拖影，只显示系统光标。
- **多选拖动移到 S3**：面板目前没有多选，等 S3 加入多选后一起实现（文本合并、文件合并成多文件 CF_HDROP、图片仍只支持单张）。

### 3.6 与现有行为的兼容

| 现有行为 | 处理 |
|---|---|
| `SetStripVisible(false)`，热键仍可用 | 不变，隐藏的是整个堆叠 |
| Win+V 打开面板 / `QuickWindow` | 不变。扇开行只显示序号 1–5：核对代码后发现面板并没有 Alt+数字快捷键（Ctrl+数字是移入分组），所以行徽标不加 "Alt+" 前缀（`SetBadgePrefix(L"")`）。**不注册全局 Alt+数字热键** |
| 连续粘贴（固定面板） | 不变；在堆叠上单击卡片时同样跟随当前输入窗口 |
| 设置中的旧样式 | 新增"侧边条样式：卡片堆叠 / 经典细条"。**所有用户（包括升级用户）默认切换为卡片堆叠**；经典细条保留现在 28×96 的液态实现，可在设置里切回，也用于对比测试 |
| 截图过程中 | 沿用现状（截图冻结前的显示逻辑不变） |
| 关闭目标 `DismissZone` | 只在拖动把手时出现；拖卡片时不出现 |

### 3.7 面板集成（S1 第二部分，已实现）

| 项 | 做法 |
|---|---|
| 样式开关 | `clipboard-layout.ini` 的 `[Strip] Style=Stack` 或 `Classic`，缺省为 Stack（升级用户也一样）。`SavePosition` 会整体重写这个文件，所以它会把 `Classic` 带过去，不会丢。设置界面的开关放到 S3（不改用户正在改的 `settings_dialog.cpp` / `preferences.*`） |
| 创建 | `Enable` → `Create` 之后 `CreateStack()`；Windows.UI.Composition 不可用时 `stack` 为空，自动沿用经典细条。测试模式默认经典细条，现有 strip 测试不变 |
| 折叠 | `Place()` 在折叠且有 `stack` 时走 `PlaceStack()`：液态细条和空闲分层条都不显示，`Present()` 在折叠时直接返回，液态细条不再渲染 |
| 位置 | 与经典细条共用 `anchor`（右边缘 x、顶端 y）：右侧 = 工作区右边 −6，左侧 = 工作区左边 + 28 DIP。两种样式来回切换位置不变 |
| 内容 | `stack_items.h`（纯函数）把 `Entry` 映射为卡片：图片、文本（Aa）、代码（&lt;/&gt;）、链接（域名首字母 + 按域名固定的调色板颜色）、`#RGB`/`#RRGGBB` 色块、文件（首个文件名 + "等 N 个"，扩展名徽标） |
| 缩略图 | 复用面板的磁盘缩略图请求；到达后在面板侧面积平均缩到约 96×84 px（覆盖 2× 的 46×40 DIP 图块），堆叠只保留这份小图；折叠时面板的 384 KB 缩略图缓存随即清空 |
| 更新合并 | `DrainStore` 不直接刷新，而是投递一次 `WM_APP+185`（停放的面板窗口要把它列入服务消息）。原因：合成器只在消息循环之间提交，连续 100 次同步刷新会让 WARP 保留所有被替换的表面，实测 +158 MiB 且不回落；合并后 100 条只画 9 次 |
| 飞入判断 | 新的首条 id 比原来的大且不在原列表里。历史满 100 条以后总数不再增长，所以不能用"总数变大"来判断 |
| 单击卡片 | `StackPaste`：记住当前前台窗口，复制并粘贴到它；堆叠从不激活，所以不恢复焦点（与快速窗口同一套 `paste_*` 流程，新增 `copy_stack/paste_stack`）。前台不是可粘贴窗口时只复制 |
| 把手 | 单击 → 打开面板；拖动 → 沿边移动并就近贴边，松开后保存位置；拖到关闭目标 → 隐藏（复用 `DismissTracker`、`DismissStrip`）；Esc → 回到拖动前的位置 |
| 空闲 | 折叠 10 s 的一次性计时器在堆叠模式下释放面板的合成和渲染器，不再进入 60 s 的空闲分层条 |

S1 第二部分实测（Windows 11，150%，同一流程、同一合成数据，每种样式一个独立进程）：

| 指标 | 经典细条 | 卡片堆叠 |
|---|---|---|
| 折叠 10 s 后 私有提交 | 47.5 MiB | **24.5 MiB** |
| 完全空闲（经典要 60 s 后切到分层条） | 22.9 MiB | 24.5–25.5 MiB（+1.6～2.6） |
| 再复制 50 条（逐条，有消息循环） | 2.5 MiB* | 11.3 MiB，未持续增长 |
| 空闲 1 s CPU | 0 ms | 0 ms |

\* 测试模式下经典细条不显示，没有绘制成本，这一行只用来确认堆叠没有持续增长。

---

### 3.8 滚轮翻看（2026-10-19）

- 在折叠的小卡片上滚动滚轮：第一格立即扇开（不等 120 ms 悬停）。
- 扇开后每格前后移动一条（向下 = 更早），5 行窗口最远到最早的 5 条为止。触控板的半格会累计。
- 翻看时：
  - 行角标显示该条在历史中的位置（6、7…），不带 `Alt+` 前缀，因为 Alt+N 只对应最新的条目。
  - 底部文字为“第 a–b 条，共 N 条”。
  - 点击、拖出操作的都是看到的那一条（面板通过 `stack_ids` 按 id 对应）。
  - 新复制的内容不会播放滑入动画。
- 收起（离开、点把手、`Collapse`）时通过 `StackEvents::folded` 回到最新，所以折叠时总是露出最新 3 张。
- 只用消息驱动，不增加计时器。只解码当前 5 条的缩略图，超出范围的会从 `stack_thumbs` 移除。
- 代码：
  - `StackWindow::SetItems(..., first)`、`StackEvents::scroll` / `folded`、`Impl::MoreText`
  - 面板：`stack_first`、`StackScroll`、`StackFolded`
- 测试：
  - `clipboard_stack_layout_test`：滚轮扇开、按格计数、半格累计、文字宽度、翻看文字、收起通知。截图模式新增 `*-paged.bmp`。
  - `clipboard_stack_panel_test`：按格翻看、两端夹住、翻看后点击、收起后回到最新。

## 4. 展开面板（按 full.html）

### 4.1 结构
```
┌ 剪贴板  Win+V · Alt+1–9      [● 记录中] [📌] [⋯] ┐
│ 🔍 搜索文本、OCR 文字、文件名…            Ctrl F │
│ [全部 8][文本][图片][文件][★收藏] [代码][链接][颜色] [分组…] │
│ 已选 3 项 ── 合并复制 · 收藏 · 删除 · ✕   （多选时出现） │
│ 刚刚                                              │
│ ┌1┐ 卡片（按 Flavor 渲染）      [悬停动作条] │
│ …                                                 │
│ 8 / 100 条 · 0.6 MB / 128 MB     ↑↓ · Enter · 空格 │
└───────────────────────────────────────────────────┘
```

### 4.2 与现有能力的对应

| full.html 元素 | 现有代码 | 改动 |
|---|---|---|
| 分类按钮 | 标签页 0–4：全部 / 文本 / 图片 / 文件 / 收藏；分组从 `GroupTabBase=5` 开始 | **保留现有索引语义**（QuickWindow 和测试依赖它）。代码 / 链接 / 颜色是附加筛选，数量为 0 时隐藏，放在分组之前；内部用映射表转换，`History::Filter` 增加一个 Flavor 参数 |
| 时间分组（刚刚 / 今天 / 昨天 / 更早） | 无 | 只是渲染层的分组标题，不影响排序 |
| 悬停动作条 | 右键菜单 | 按类型提供：图片 = 钉图 / OCR / 标注后钉图 / 复制；文本类 = 复制 / 纯文本粘贴；链接 = 打开；文件 = 在资源管理器中显示；所有类型都有收藏和删除 |
| 多选 + 合并复制 | 无（功能审查 P1 #5） | Ctrl / Shift 单击；删除提供撤销 |
| 快速预览 | `preview_window`（已有异步预览、取消机制和资源限制） | 只改外观：标题条和 OCR 文本区 |
| 记录中 / 已暂停 | 无（功能审查 P0 #2） | 暂停期间 `WM_CLIPBOARDUPDATE` 直接忽略，已有历史仍可搜索和粘贴 |
| 容量条 | `History::MaxEntries=100`、`MaxBytes=128 MiB` | 显示 **128 MB**（full.html 写的是 64 MB，`mcp-clipboard-feature-review.md` 里的 64 MiB 也已过时，一并更正） |
| 卡片拖动 | 无 | §3.5：**所有卡片的整个区域**都可以拖（full.html 只有图片缩略图能拖） |
| 面板最小尺寸 | `layout.h`：`PanelMinWidthDip=280`；`clipboard-shortcuts.md` 原写 400×480 | 以代码为准，S0 已修正文档。最小宽度**保持 280 DIP**：`mcp-clipboard-panel-width.md` 是有意调低并在 280 下验证过的，不回退到 320；新卡片布局（S3）必须在 280 DIP 下完整可用 |

### 4.3 折叠 ↔ 展开的连续性
点击把手时，堆叠中的 3 张卡片在 `StackWindow` 内用合成动画飞到面板列表第 1–3 张卡片的位置（动画期间临时扩大窗口，覆盖飞行路径），同时面板沿现有的 `LiquidTween` 打开（350 ms）。收起时反向，卡片落回堆叠。用户看到的是 **同一组卡片换了排列**，而不是"一个条变成另一个窗口"。

**实现记录（S3 第三部分，2026-10-17）**
- `StackWindow::Fly(slots, toward_panel)`：`slots` 是面板中显示堆叠条目的卡片的屏幕矩形（物理像素，按堆叠顺序），由 `ClipboardPanel::StackSlots()` 按条目 id 在当前列表（含滚动、分组标题）中查找；不在可视区的条目没有落点，只随飞行淡出 / 淡入。
- 全部是 DWM 合成动画（`Offset / Size / CenterPoint / Rotation / Scale`、圆角裁剪、玻璃层），350 ms，缓动用 full.html 的 `--ease-spring` `cubic-bezier(.25,1.2,.4,1)`；应用侧只有一个 390 ms 的单次定时器（`FlightTimer`）负责收尾，没有逐帧循环。
- 飞行期间视觉窗口临时扩大到"堆叠矩形 ∪ 落点 + 56 DIP 阴影余量"，根视觉反向偏移保持卡片原位；输入窗口不动。收尾时窗口缩回、根偏移清零、按当前状态重新摆放。
- 展开：面板先显示，再把视觉窗口提到最上层开始飞行；卡片先变成"展开行"样式（玻璃、缩略图、标题），再飞到落点，在最后 28% 淡出，露出面板自己的卡片。期间的 `Show(false)` 只隐藏输入窗口，视觉窗口等落地后再隐藏。
- 收起：`Fold()` 在面板消失前记录落点，`PlaceStack()` 显示堆叠后，卡片从面板落点（10 DIP 圆角、无旋转）飞回折叠姿态，前 25% 淡入。
- 不飞行的情况：系统关闭"窗口内动画"、测试模式、面板未启用、堆叠不可见或正在飞行。`Place()`（换位置 / DPI）和 `Show(true)` 会立即结束进行中的飞行。
- **偏差**：堆叠模式下面板本身不走 `LiquidTween` 形变（折叠条已隐藏，没有起点形状），连续性完全由飞行的卡片承担。

---

### 4.4 S3 第一部分实现说明（已实现）

- **内容类型**：新文件 `src/clipboard/entry_flavor.h`，定义 `Flavor{Text,Code,Link,Color,Image,Files}`，判断顺序为颜色 > 链接 > 代码 > 文本，堆叠也使用同一套规则。
  - `TextFlavor` 不复制文本，因为单条最多 16 MB。
  - 面板按条目 id 缓存类型结果。
- **类型筛选**：`History::Filter(tab,query,oldest,flavor)` 新增可选的 flavor 参数，标签页索引 0–4 和分组的含义都不变。
  - 文本 = 标签页 1（包含所有文本类型）。
  - 代码 / 链接 / 颜色 = 标签页 1 + flavor（动作 26，id = 1 + flavor），数量为 0 且未被选中时隐藏。
  - `SelectTab` 和 Alt+←/→ 会清除 flavor。
- **几何**：全部来自 `src/clipboard/panel_cards.h`（纯函数）。
  - 头部 0–54；搜索框 56–92；筛选标签 92–115；列表从 121 开始；页脚 45。
  - `.item` 内边距 9 / 10 / 8 / 32，卡片间距 6，圆角 10。
  - 正文：文本最多 3 行 × 19.5；代码最高 76；链接 / 颜色 / 文件为 38；图片 ≤150、不放大、最小 48。
- **滚动（S3 第二部分，已实现）**：按像素滚动，与 full.html 的 `#list` 一致。
  - 列表由"块"组成：可选的时间分组标题（30）+ 卡片 + 间距 6。底部留 10 px（`#list` 的 padding-bottom）。
  - `scroll` 仍是顶部块的索引，`scroll_offset` 是块内的像素偏移，只在 `offset_index==scroll` 时生效。这样直接按索引赋值的代码和测试含义不变。
  - 滚动位置对齐到整设备像素，保证文字清晰；滚到两端时精确停在 0 和最大值。
  - 滚轮：每格滚动"系统滚动行数 × 33 DIP"（默认 3 行约 100 DIP，与浏览器一致）。
    - 用定时器 12 按指数缓动（τ = 60 ms）播放，只在滚动过程中运行，停下即关闭。
    - 系统关闭"窗口内动画"时直接跳到目标位置，测试中也是如此。
  - 键盘移动选中项时：向上露出卡片及其分组标题，向下在卡片下方留 10 px（同 full.html）。
  - 列表上下边缘的卡片可以只显示一部分（裁剪绘制）。它们的点击区域也被裁剪在列表内，不会抢走筛选标签和页脚的点击。
  - 只为与视口相交的卡片（`DrawnRange`）保留缩略图。
  - `PageRows()` / `FitCount()` 表示完整可见的卡片数，用于 PageUp/PageDown 和滚动条；滚动条按像素计算。
- **缓存键**：卡片高度、内容类型、代码片段三个缓存按"条目 id + 内容指纹"作键。
  - 指纹是对类型、长度、格式数量和最多 80 个字符做 FNV-1a，不读取整段文本。
  - 清空历史后 id 会被复用，只用 id 作键会拿到旧条目的高度和类型（曾导致长文本只显示 1–2 行，纯文本历史出现"代码 / 链接"标签）。
- **文本三行截断**：截断后的文本布局多留半行高度。否则开启省略号截断时，DirectWrite 会因浮点误差丢掉第 3 行。
- **时间分组**：
  - 1 小时内为"刚刚"，然后是今天 / 昨天 / 更早。
  - 置顶的收藏单独成组，标题为"★ 收藏"。
  - 每页第一张卡片总是带分组标题。
- **卡片**：
  - `.idx` 徽标（1–9 为强调色）。
  - 正文按类型绘制：代码复用 `HighlightCode`，片段缓存；颜色取 full.html 的 `.kw` / `.str` / `.cm` / `.nb` / `.fn`。
  - `.meta` 两行：类型或语言徽标 + 详情；★ + 时间 + 分组。
  - `.acts` 动作条只出现在悬停卡片和当前卡片上，按钮 28 DIP。按类型给出动作（同 full.html 的 `actsFor`，S3 第二部分已实现），后面固定跟分组 / 收藏 / 删除：

    | 类型 | 动作（动作码） |
    |---|---|
    | 图片 | 钉到桌面 36 · OCR 37 · 标注后钉图 38 · 复制 32 |
    | 文本 / 代码 | 复制 32 · 纯文本粘贴 45（同 Shift+Enter） |
    | 链接 | 用浏览器打开 39（只打开 http/https，`www.` 补 https）· 复制 32 |
    | 文件 | 在资源管理器中显示 42 · 复制路径 43（多个文件按行分隔） |
    | 颜色 | 复制 HEX 44（统一为大写 `#RRGGBB`） |

    - 钉图 / OCR / 标注把解码后的图片交给宿主：`SetPinHandler`、`SetImageActionHandler(ClipboardImageAction::Ocr / Annotate)`。宿主没有提供时，对应按钮不显示。新钉图放在面板左侧（放不下就放右侧），限制在工作区内。
    - 复制路径、复制 HEX 只写系统剪贴板，并更新剪贴板序号，不会再被记成一条新历史。
    - 原来的"粘贴"按钮从动作条移除（双击 / Enter 仍是粘贴）。
- **过渡（S3 第二部分，已实现）**：对应 full.html `.item` 的 `background / border-color .12s` 和 `.acts` 的 `opacity .12s + translateY(-3px)`。
  - 每张卡片记录悬停、选中、动作条三个进度值，用 smoothstep 缓动，120 ms 完成。
  - 定时器 13 只在有值变化时运行，全部到位后立即关闭；值都回到 0 的卡片会被移出表，不常驻内存。
  - 动作条淡出时仍会绘制，但不接收点击（同 CSS 的 pointer-events）。
  - 系统关闭"窗口内动画"或在测试中时直接跳到终值；折叠面板时清空过渡状态。
- **头部**：Logo、"剪贴板"、快捷键行、记录中 / 已暂停胶囊（动作 15）、固定 / 设置 / 更多（30 DIP）。宽度 <330 时胶囊只显示圆点。
- **暂停**：
  - 暂停时 `Read()` 只更新序号，不记录内容，并提示"记录已暂停"。
  - 恢复时不导入暂停期间复制的内容。
  - 状态保存在 `[Clipboard] Paused`，`SavePosition` 重写文件时保留它。
  - 堆叠通过 `SetPaused` 显示暂停状态。
- **页脚**："N / 100 条 · X.X MB / 128 MB"，右侧显示提示或状态，下方是 4 px 容量条。
- **与原型的偏差**：
  - 文件卡片用真实的系统图标（文件夹、exe 也能正确显示），没有用原型里的扩展名角标。
  - "文本"标签的计数包含代码 / 链接 / 颜色。
  - 保留"设置"按钮、分组"+"和排序按钮（原型没有，但功能需要）；卡片动作条也保留"分组"按钮。
  - 链接标题显示域名，因为没有网页标题数据；来源应用没有数据，第二行元信息显示分组。
- **多选（S3 第三部分，已实现）**：Ctrl+点击切换、Shift+点击加选、Ctrl+A 全选、Esc 清除。选中 2 张及以上时列表上方出现批量栏（列表下移 40 DIP）：合并复制 50（文本按行合并，文件合并为一个 `CF_HDROP`）、收藏 51、删除 52、清除 53；拖动任一选中卡片会拖出合并内容。
  - 批量删除先从列表隐藏，6 秒后（定时器 14）或折叠面板时才真正删除；期间 Ctrl+Z / "撤销"（54）可恢复。
  - **偏差**：第一次 Ctrl+点击会把当前选中的卡片一起选入；撤销只针对批量删除；Shift+点击是加选，不是区间选择；Ctrl+Z 依赖 `GetKeyState`，无法做单元测试。
- **Quick Look（S3 第三部分，已实现）**：按 full.html `#ql` 重做：42 DIP 头部（类型图标、名称、"· HH:MM · 宽 × 高"、"空格 关闭"键帽，文本类显示语言），34 DIP 页脚提示；宽 440，高度按类型（链接 190、颜色 300、其它 460）。
  - 图片：阴影 + 8 DIP 圆角裁剪，下方 OCR 区（"按 O 识别图中文字" / "正在识别文字…" / "OCR：识别结果"）；按 O 由面板处理（预览窗口不激活），结果按条目在本次会话中缓存（最多 32 条）。
  - 颜色：色块 + HEX；链接：图标块、域名、完整 URL；文本用 14 DIP 正文字体，代码保持等宽。
  - 切换图片时，新图解码完成前保持当前窗口尺寸（缩略图 / 加载态不改变大小），避免闪动和重建渲染目标。
  - **偏差**：没有 `qlin` 入场动画；窗口圆角用 DWM 圆角（约 8 px），不是 14；文件内容区未重做；图片不放大（最大 100%），窗口最小宽 440。
- **新条目滑入（`.item.new`，S3 第三部分，已实现）**：面板展开时新记录的卡片 450 ms 内从 `translateY(-12px) scale(.96)`、透明，弹到原位（`cubic-bezier(.25,1.2,.4,1)`，二分求解），复用定时器 13，结束即停。
  - **偏差**：关闭动画、测试模式或面板折叠时不播放。

## 5. 模块联动

所有连线都由 `Application` 负责转接，模块之间不互相包含头文件，保持现有边界。

### 5.1 截图 → 堆叠（飞入）
- 截图完成并复制后，`Application` 调用 `clipboard_panel_->AnnounceIncoming(selection_rect, GetClipboardSequenceNumber())`。
- 面板在 2 s 内的下一次 `WM_CLIPBOARDUPDATE` 中，如果序号匹配，Incoming 动画的起点改为选区位置：`StackWindow` 临时扩大到覆盖飞行路径，新卡片从选区位置沿曲线飞到堆叠（合成动画，280 ms），结束后窗口恢复原尺寸。
- 序号不匹配（例如被其他程序抢先写入剪贴板）时，只播放普通 Incoming。

### 5.2 卡片 → 钉图
```cpp
// panel.h
struct PinRequest { std::shared_ptr<const Frame> image; POINT at; RECT from; bool annotate{}; };
void SetPinHandler(std::function<void(PinRequest)> handler);
void SetOcrHandler(std::function<void(std::shared_ptr<const Frame>, uint64_t entry)> handler);
```
- 触发入口：拖到桌面、悬停动作"钉图 / 标注后钉图"、右键菜单。
- 原图在工作线程解码（复用 `preview_data` 的解码路径，不在 UI 线程解码大图）。
- `Application` 收到后调用 `pins_->Create(image, image, at, ...)`；`annotate=true` 时创建后立即进入标注模式。
- `from` 用于钉图从卡片位置飞出（`PinManager` 已有 `FlyStep`）。

### 5.3 钉图 → 堆叠（加入历史）
```cpp
// pin.h（PinManager 新增回调）
std::function<bool(POINT screen)> over_clipboard_stack;   // 命中测试
std::function<void(bool)>         clipboard_stack_hover;  // 让堆叠显示插入空位
```
- 钉图拖动时，`PinManager` 每次移动都询问 `over_clipboard_stack`。命中时把钉图淡化（alpha 150，与晾衣绳一致）。
- 松手后钉图回到原位，同时调用现有的钉图复制路径：在导出工作线程准备 DIB → `WM_CLIPBOARDUPDATE` → 进入历史 → 播放 Incoming。不需要新的数据通道。
- 晾衣绳（顶部）和堆叠（侧边）区域不重叠，两者的判断互不冲突。

### 5.4 钉图悬停栏（按 full.html `.pin .hbar`）✅ 已实现
- 形式：钉图拥有的独立分层小窗口（不抢焦点、置顶），浮在图片**外**上方 8 DIP、与图片右边对齐；屏幕上方放不下时放到图片下方 8 DIP（`.hbar.below`），始终限制在工作区内。钉图移动、缩放时跟着走。
- 显示：鼠标在钉图或栏上时淡入（160 ms，透明度 + 5 px 上滑，同原型）；从钉图移到栏之间有 250 ms 宽限；标注中、拖动中、锁定、晾在绳上、关闭动画中不显示。
- 外观（按原型 token，跟随深浅色主题）：玻璃底 `#f5f8fc` / `#1c2532`，图标色 `#18212d` / `#e9eef5`，圆角 12，3 DIP 内边距，30 DIP 按钮（16 DIP 图标，路径照抄原型 SVG），悬停底色，按下缩小到 0.94，关闭前有竖分隔线，带柔和阴影。
- 按钮与提示（提示悬停 380 ms 后出现在栏的外侧，按键用键帽显示）：

  | 按钮 | 提示 | 行为 |
  |---|---|---|
  | 标注 | 标注 [空格] | 进入钉图标注；没有标注处理器时不显示 |
  | 识别文字 | 识别文字 / 正在识别文字… / 复制识别出的文字 | 未识别：开始 OCR；已识别且有文字：复制全文，提示"已复制全部文字" 1.4 s；无 OCR 引擎时不显示 |
  | 复制 | 复制图片（加入历史） | 复制钉图当前显示的图片（含文字标记、译文），剪贴板历史照常收录；提示"已复制 · 已加入剪贴板历史" 1.4 s |
  | 晾 | 晾到绳上 [H] | 同 H；设置里关闭晾衣绳时不显示 |
  | 更多 | 更多 | 在按钮下方弹出贴图菜单（同右键菜单；末组含“加入剪贴板历史”“晾到绳上 [H]”，见 `docs/pin-menu.md`）；栏上右键也弹这个菜单 |
  | ｜ 关闭 | 关闭 | 淡出关闭 |

- 栏在图片外，所以不再按钉图宽度删减按钮（小钉图也是完整一排，和原型一致）。
- 钉图自己的画布上没有任何按钮，复制、保存、拖出的图片自然不含按钮。
- 代码：`src/pin/pin.cpp`（`PinTool`、`Pin::ToolLayout`、`HoverBarRect`、`PlaceHoverBar`、`DrawHoverBar`、`PaintTools`、`HoverBarProc`、`HoverTool`、`DrawPinToolIcon`、`PinToolLabel/Key`），`src/pin/pin.h`。按钮 id 0=晾、1=关闭保持不变。
- 测试：`tests/clothesline_test.cpp` "Hover bar window" 段；预览图 `build/toolbar-customization/pinbar-shots/`（bar、hover-*、below）。
### 5.5 面板图片 → OCR
- `Application` 把请求交给应用级的快速 OCR（`quick_ocr_`，任务 id 2，新请求先取消旧的），不新建 OCR 实例。（偏差：方案原写复用 `PinManager` 的 `service_`；`quick_ocr_` 是同一个已加载的服务入口，且不需要改 `pin.*`。）
- 识别结果写入剪贴板（会自动进入历史），托盘提示"识别到 N 行，已复制"；同时通过 `ClipboardPanel::SetImageOcrResult` 回传面板，状态栏显示"已识别文字并复制" / "未识别到文字"，Quick Look 的 OCR 区显示结果。

### 5.6 面板图片 → 标注后钉图
- 动作 38 复用现有的 `AnnotateImage` 编辑器，确认后 `Flatten` 并 `pins_->Create` 生成钉图；面板随即折叠，避免挡住编辑器。

---

## 6. 代码改动清单

| 文件 | 类型 | 内容 | 估计行数 |
|---|---|---|---|
| `src/ui/luma_glass.h` | 新增 | §2 的 Token | ~80 |
| `src/clipboard/stack_layout.h` | 新增 | 纯几何：折叠 / 扇开位姿、输入多边形、DPI 换算、左右镜像、贴边延伸 | ~200 |
| `src/ui/backdrop_compositor.{h,cpp}` | 新增 | DispatcherQueue、Compositor、桌面窗口目标、HostBackdrop 亚克力画刷配方、`IGraphicsEffect` 实现、退路判断 | ~450 |
| `src/clipboard/stack_window.{h,cpp}` | 新增 | 卡片视觉树与合成动画、输入区域、悬停状态机、Incoming、拖动发起 | ~850 |
| `src/clipboard/entry_flavor.h` | 新增 | `DetectFlavor` | ~90 |
| `src/clipboard/card_render.{h,cpp}` | 新增 | 迷你卡片 / 行卡片 / 面板卡片的 D2D 绘制（文字走 TextRenderer），供堆叠和面板共用 | ~500 |
| `src/clipboard/panel.cpp` | 修改 | 折叠路径改为由 `StackWindow` 负责；面板列表改用 `card_render`；多选、暂停、附加筛选、卡片拖出；新回调 | 改动约 600 |
| `src/clipboard/panel.h` | 修改 | `SetPinHandler` / `SetOcrHandler` / `AnnounceIncoming` / `StackContains` / `SetStackDropHover` | +20 |
| `src/clipboard/history.h` | 修改 | `Entry::flavor`（运行时字段）；`Filter` 增加 Flavor 参数 | +30 |
| `src/export/drag_out.{h,cpp}` | 修改 | `DragPayload` / `DragOut` / `on_release`；文本、HTML、RTF、URL、虚拟文件格式 | +250 |
| `src/pin/pin.{h,cpp}` | 修改 | 悬停栏扩展；`over_clipboard_stack` 回调 | +200 |
| `src/app/application.cpp` | 修改 | 4 条连线 | +60 |
| `src/app/preferences.*`、`settings_dialog.cpp` | 修改 | 侧边条样式；显示内容预览 | +60 |
| `CMakeLists.txt` | 修改 | 新源文件与测试目标 | +30 |
| `docs/clipboard-card-stack.md` | 新增 | 本方案定稿后的技术说明 | — |

`panel.cpp` 目前有 1618 行且代码密集。本方案的原则是 **新功能放在新文件**，`panel.cpp` 只做接线，借这次重构让它变小，不再变大。

---

## 7. 分阶段实施

每个阶段结束都执行：`build.bat` → 相关测试 → 构建 `dist/LumaShot-Setup.exe` → 报告 SHA-256（不安装）。

| 阶段 | 内容 | 新增 / 更新测试 | 验收 |
|---|---|---|---|
| **S0 Token、文档校准、性能基线**（1 天） | `luma_glass.h`；现有常量改为引用；修正 64 MiB 和 400×480 两处文档；**按 §1.3 记录性能基线**，并在测试中加入计时 / 内存 / 唤醒埋点 | 现有外观回归（数值不变，截图应一致）；性能基线报告 | 截图逐像素一致；基线数据存放在 `build/perf-baseline/` |
| **S1 卡片堆叠**（Spike 2 天 + 实现 5 天） | Spike 5 项（§3.3）通过后才开始；`backdrop_compositor`；`stack_layout` / `stack_window` / `card_render`（迷你卡片 + 行）；Rest / Fan / Incoming / Idle / MoveStack / Hidden；单击粘贴、点把手展开；设置项 | `clipboard_stack_layout_test`（位姿、多边形、100/150/200%、左右侧、相邻显示器退路）；`clipboard_strip_test` 新增样式分支；外观截图（双主题、双侧） | §1.2 第 1、3、4 条；没有新增计时器（用现有 `idle_memory_test` 的方法审计） |
| **S2 全卡片拖出**（3 天） | `DragOut` 扩展；桌面拦截 → 钉图；拖回置顶；面板卡片整卡可拖；多选拖动 | `clipboard_drag_out_test`：用合成 Entry 构造数据对象并逐个检查格式；`on_release` 决策表；失效路径；不读真实剪贴板 | 6 种类型都能拖到记事本 / 浏览器 / 资源管理器（手动测，记录在 `build/`） |
| **S3 面板改版**（4 天） | Flavor 卡片；时间分组；附加筛选；悬停动作条；多选与批量操作；暂停记录；容量条；快速预览外观；折叠 ↔ 展开的卡片飞行 | `clipboard_panel_test`、`clipboard_width_test` 更新；`entry_flavor_test`；`clipboard_history_test` 增加 Flavor 筛选 | 与 full.html 对照截图评审 |
| **S4 联动**（3 天） | §5 的四条连线；钉图悬停栏 | `capture_pin_test`、`clothesline_test` 回归；新增 `pin_stack_link_test`（合成钉图拖到合成堆叠矩形） | 截图 → 飞入 → 拖到桌面钉图 → 钉图拖回堆叠，整条链路走通 |
| **S5 打磨与审计**（2 天） | 高对比度、减少动画、UIA 名称（卡片标注"第 N 条，类型，来源"）、内存与计时器审计、`docs/clipboard-card-stack.md` | `clipboard_resource_audit_test`、`clipboard_disable_memory_test` | §1.2 全部 |

总计约 20 个工作日（估算，包含 Spike 和性能基线）。每个阶段的交付报告都包含 §1.3 的性能对比表。S1 和 S2 完成后就已经交付"卡片堆叠 + 全卡片可拖"这一核心体验，之后各阶段都可以单独发布。

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| HostBackdrop 与输入区域、DPI 或某些显卡驱动不兼容 | 卡片玻璃显示异常 | Spike 第 1、2 项提前暴露；运行时检测到失败就使用实色退路（形状和动画不变） |
| 卡片玻璃与面板系统亚克力看起来不一样 | 再次出现割裂感 | ΔE ≤ 3 的自动对比测试；必要时面板也改用同一配方 |
| 引入 Windows.UI.Composition 增加内存 | 违背轻量目标 | Spike 第 5 项设定预算；空闲时释放绘制设备 |
| 旋转卡片的输入区域不精确 | 边缘点击穿透或误点 | 多边形区域外扩 2 DIP；`stack_layout` 单元测试覆盖四个角 |
| 桌面判断（Progman / WorkerW）在第三方桌面或多显示器下失效 | 图片拖到桌面没生成钉图 | 判断失败时一律按"存成文件"处理（安全退化）；Alt 反转行为 |
| 提权程序（UIPI）拒绝来自普通权限进程的拖放 | 拖进去没反应 | 结果为 NoTarget 且不在桌面 → 自动复制并提示 Ctrl+V |
| 屏幕边缘显示内容，有隐私顾虑 | 共享屏幕时泄露 | "显示内容预览"开关；暂停记录；隐私格式本来就不记录（`native.cpp:14-18`）。**不使用 `SetWindowDisplayAffinity`**（已知 `WDA_NONE` 会导致隐藏亚克力的残影问题） |
| 悬停误触发扇开 | 打扰 | 进入需停留 120 ms，离开有 300 ms 宽限；拖动其他窗口时（左键按下中）不扇开 |
| 内存上升 | 违背轻量目标 | 迷你卡片缩略图最长边 96 px，进入 PreviewCache 统一预算；空闲 60 s 释放绘制设备；Spike 第 5 项设定硬预算 |
| `panel.cpp` 改动面大 | 回归 | 新功能放新文件；经典细条作为可切换的回退，方便对比和二分定位 |

---

## 9. full.html 中需要修正、不照搬的点

1. **代码高亮有 bug**：`codeHL()` 先插入 `<span class="cm">`，后面的字符串正则又匹配到了 `"cm"`，界面上出现了 `class=class="str">` 这样的残片（截图中第 1 张卡片可见）。原生实现使用 `HighlightCode` 返回的 spans，不存在这个问题，原型可以顺手修掉。
2. 悬停动作条会盖住链接卡片的标题。原生实现给卡片右上角预留 `动作条宽度 + 8` DIP，或者让标题在悬停时截断。
3. 只有图片缩略图能拖。按本方案改为 **整张卡片、所有类型都能拖**。
4. 容量显示为 64 MB，实际上限是 128 MiB。
5. 没有"分组"和"连续粘贴"。这两项是现有功能，必须保留（分组标签页从 `GroupTabBase=5` 开始）。
6. 侧边条是一个整块，没有卡片堆叠。按本方案替换为 D 卡片堆叠，`strip-concepts.html` 的 D 标签页可以直接演示：卡片拖到程序、桌面、回堆叠，钉图拖回堆叠，把手拖动换边。

---

## 10. 已确认的决定（2026-10-17）

| 问题 | 决定 |
|---|---|
| 扇开效果 | **必须达到 HTML 原型的效果**。采用 §3.3 做法 C（合成层逐卡亚克力），否决"一张底板"；Spike 任何一项不通过都要先确认，不降级 |
| 图片卡片拖到桌面 | 默认生成钉图，按住 Alt 存为文件 |
| 默认样式 | 所有用户（包括升级用户）默认使用卡片堆叠，经典细条可在设置里切回 |
| 文档 | 写入仓库 `docs/clipboard-card-stack.md` |
| 性能 | **速度要快、占用要低**，作为硬约束：§1.3 的预算超标就不算完成；Spike 先量化玻璃效果的开销 |

---

## 11. 参考

- `DWMWA_USE_HOSTBACKDROPBRUSH`（让非 UWP 窗口使用 host backdrop 画刷，Windows 11 22000 起支持）：https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
- `Compositor.CreateHostBackdropBrush`（采样窗口后方、本窗口绘制之前的画面）：https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositor.createhostbackdropbrush
- 系统圆角与系统背景（面板现状依据）：`docs/clipboard-system-corners.md`
- 折叠态内存方法与基线：`docs/clipboard-folded-memory.md`
- 钉图拖出（OLE）现有实现：`docs/pin-clothesline.md`、`src/export/drag_out.{h,cpp}`
- 交互原型：仓库根目录 `full.html`（面板）；`strip-concepts.html` 的 D 标签页（卡片堆叠与拖动规则；设计稿，未放入仓库）

## 修复：收起时图片卡片不显示缩略图（2026-10-20）

**现象**：卡片堆收起时，每张图片卡片都显示同一张日落渐变占位图，分不清是哪张图。

**原因**：
- 图片缩略图从历史存储异步读取（`SessionStore::Load(..., thumbnail=true)`），所以图片卡片总是在缩略图到达之前就建好了，先画的是占位图。
- 缩略图到达后，`ClipboardPanel::RefreshStack` 确实把带缩略图的条目传给了 `StackWindow::SetItems`。
- 但 `Sync` 复用同一 id 的卡片时只重画标签，从不重画缩略图，占位图就一直留着。

**修复**（`src/clipboard/stack_window.cpp`）：
- `CardVisual` 记录 `thumb_key`，即绘制缩略图所用内容的签名：类型、强调色、字形、缩略图尺寸和像素抽样。
- `Sync` 复用卡片时，如果签名变了就重画缩略图。这覆盖两种情况：缩略图异步到达，以及“卡片显示内容预览”切换。
- 签名不变时不会重画。
- `Diagnostics::thumbnail_cards` 统计缩略图里真正画着图片的卡片数量。

**测试**（`clipboard_stack_panel`，92 项检查，0 失败）：
- 图片卡片先显示占位图；
- 缩略图到达后，收起的卡片显示真图；
- 已显示的缩略图不重复重画；
- 关闭内容预览后缩略图被隐藏，重新打开后又显示。
