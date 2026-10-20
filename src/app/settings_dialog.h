#include "ui/shortcut_label.h"
#pragma once
#include "app/preferences.h"
#include <functional>
#include <string>
namespace lumashot {
bool ShortcutModifier(UINT key);

// 恢复本页默认: restores one settings page (0 常规 … 6 外观); 7 关于 has nothing to reset.
void ResetSettingsPage(Preferences& value,int page);
bool ShowSettingsDialog(HWND owner,Preferences& value,const std::function<bool(const Preferences&)>& accept,const std::function<void(bool)>& recording_changed = {},const std::function<void()>& check_updates = {});
}