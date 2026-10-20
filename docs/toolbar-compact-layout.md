# Compact toolbar layout

The capture toolbar and its property card both size themselves to their content, so opening a tool's properties no longer leaves large gaps between buttons. The approved HTML mock-up was delivered separately and is not part of the repository.

## Bar

Applies to the icon-only and automatic label modes.

- **Size:** the bar is exactly as wide as its buttons.
  - Tools use 36 DIP slots, each holding a 32 × 32 DIP button.
  - Actions use 32 DIP slots, each holding a 30 × 32 DIP button.
- **Groups:** the buttons form four groups:
  1. Tools.
  2. 选择 / 撤销 / 重做.
  3. Output actions: 保存, 复制, then the quick actions in the user's order.
  4. 取消 / 完成.

  Neighbouring groups are separated by a 12 DIP gap with a 1 × 20 DIP divider in the middle.
- **Unavailable quick actions:** these are OCR, translation, long capture and recording while editing a pin. They take no space; the caller passes the availability to `PlaceToolbar`.
- **Fallback:** if the content-sized bar does not fit the monitor (monitor width − 16 DIP), the previous squeezed or wrapped grid is used. The 图标 + 文字 label mode always uses the previous grid.

With the default seven tools and five quick actions, the bar is 686 DIP wide.

## Property card

- **Captions:** each group's caption sits to the left of its controls (12 DIP, muted colour).
- **Height:**
  - Each row is 34 DIP high, with 28 DIP fields centred in it.
  - With 6 DIP top and bottom padding, a one-row card is 46 DIP tall.
- **Spacing:**
  - 2 DIP between the fields in a group.
  - 8 + 1 + 10 DIP around the divider between groups.
- **Segmented controls:** two- or three-way choices share one rounded track, and their buttons are 24 DIP tall. These are 轨迹 and 模式 for the pen, 模式 and 方式 for mosaic, and 对齐 for text.
- **No stretching:** every field keeps its natural width.
  - The card is its content width plus 8 DIP on each side.
  - The card starts under the active tool button and is clamped to the monitor.
- **Wrapping:** only when the content is wider than the monitor (monitor width − 16 DIP − padding).
  - Groups then continue on a new row 40 DIP lower.
  - The new row is left-aligned and has no leading divider.

## 属性栏样式 (property bar style)

Settings → 工具面板 → 显示 → 属性栏样式 picks how the property card is laid out. It is stored as `[Toolbar] PropertyStyle` (0 or 1) and carried over the settings IPC (exchange version 20).

- **窄条 (strip, default):** the layout described above, with one row and each caption to the left of its controls.
- **卡片 (card):** each caption sits above its controls.
  - A group is as wide as the wider of its caption and its fields.
  - A row is 48 DIP: an 18 DIP caption band, then a 28 DIP field band starting 20 DIP from the row top.
  - The card has 8 DIP top and bottom padding, so a one-row card is 64 DIP tall.
  - Wrapped rows are 58 DIP apart.
  - Gaps, dividers, segmented tracks and the no-stretching rule are the same as the strip.
- **Shared behaviour:** both styles offer exactly the same controls and popup anchors. The main bar does not change.

## Fixes made along the way

`ToolbarPropertiesId` (the 属性 button in manual mode) was 23. That is also the id of the fourth colour swatch, so clicking green toggled the property card instead of picking the colour. It is now 88, and `toolbar_test` checks that the button's id never matches a property control.
