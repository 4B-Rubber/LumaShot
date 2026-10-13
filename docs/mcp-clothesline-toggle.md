# 晾衣绳总开关（设置 → 外观 → 顶部晾衣绳）

## 需求
用户希望能在设置里彻底关闭「晾衣绳」效果。

## 行为
- 设置 → 外观页新增卡片「顶部晾衣绳」（控件 id 147，拨动开关），默认 **开**。
  说明文字：「把贴图拖到屏幕顶部或按 H 晾起；关闭后已晾起的贴图放回原处」。
- 关闭并保存后立即生效：
  - 已晾在绳上的贴图依次飞回各自原位（复用 `RetrievePin` 的飞回动画），绳子收起；
  - 屏幕顶部不再检测停留，不显示顶部光晕、拉环和「晾着 N 张」提示（顶部轮询停止）；
  - 拖动贴图到顶部不再出现插槽，松手也不会晾起；
  - 贴图上的 H 键无效，悬停按钮只剩「关闭」，「晾到绳上 (H)」按钮隐藏；
  - 重启恢复会话时，原本晾着的贴图直接显示在原位。
- 重新打开后恢复以上全部功能；「恢复默认」也会把它重新打开。

## 实现
| 文件 | 改动 |
| --- | --- |
| `src/app/preferences.h/.cpp` | `clothesline_enabled{true}`，INI 键 `General/ClotheslineEnabled`（缺省 1） |
| `src/app/settings_process.cpp` | IPC `Values` 增加字段；`Decode` 拒绝 >1；Exchange 版本 14 → 15 |
| `src/app/settings_dialog.cpp`, `resources.rc` | 控件 147：外观页布局、绘制、切换、恢复默认 |
| `src/pin/clothesline.h/.cpp` | `SetEnabled(bool)` / `Enabled()`；关闭时 `Armed/Arm/Show/DragUpdate/DragDrop/Hang/Restore` 均不生效，`SetEnabled(false)` 会 `RetrieveAll` + 收起绳子和光晕 + 停止轮询 |
| `src/pin/pin.h/.cpp` | `clothesline_enabled` 回调、`ClotheslineConfigChanged()`、`LineOn()`；`Pin::hang_tool` 控制悬停按钮 |
| `src/app/application.cpp` | 绑定回调；启动时与保存设置后调用 `ClotheslineConfigChanged()` |

## 测试
- `settings_dialog`：开关默认开、切换、位于外观页且在页脚之上、恢复默认、INI 往返、缺省键为开、IPC 往返（开/关）、IPC 拒绝非法值 2。
- `pin_clothesline`：`Clothesline::SetEnabled` 关闭后全部飞回、Hang/Drag/Drop/Restore/Show 被忽略、不再轮询、重新开启恢复；
  PinManager：关闭后已晾贴图飞回原位、H 无效、只剩关闭按钮、旧按钮位置点击无效、拖到顶部不晾、重新开启后恢复。
