#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include "model/tool_properties.h"
#include "model/pin_style.h"
#include "model/toolbar_customization.h"

namespace lumashot {
inline constexpr int ClipboardLimitChoices[]{20,50,100,200};
constexpr bool ValidClipboardLimit(int value){for(int choice:ClipboardLimitChoices)if(value==choice)return true;return false;}
struct Preferences {
    ToolProperties tools=[] {ToolProperties value;value.width=2;return value;}();
    bool include_cursor{true};
    bool ocr_remove_whitespace{false};
    std::filesystem::path ocr_model_directory;
    bool start_with_windows{true};
    bool paste_as_file{true};
    bool clipboard_enabled{false};
    // Off by default: history lives only in this session and saved payloads are purged.
    bool clipboard_persist{false};
    // The folded edge strip; hidden by dragging it onto the close target, restored
    // from the tray or settings. The hint about restoring it is shown only once.
    bool clipboard_strip_visible{true};
    bool clipboard_strip_hint_shown{false};
    // 最多保留: ordinary history entries kept (one of ClipboardLimitChoices).
    int clipboard_max_entries{100};
    // Folded side strip: card stack (default) or the classic thin strip.
    bool clipboard_classic_strip{false};
    // Stack cards show thumbnails and text; off shows only the type tile.
    bool clipboard_card_preview{true};
    // Switch the pin to the translated rendering as soon as a translation finishes.
    bool translate_auto_show{true};
    // Top-edge clothesline (drag a pin to the screen top or press H). Off returns hung pins home.
    bool clothesline_enabled{true};
    // Last section shown in the settings window (0 shortcuts … 5 general).
    int settings_page{};
    bool hotkeys_disabled{false};
    bool disable_hotkeys_in_game{false};
    int paste_file_format{}; // 0: PNG, 1: JPEG, 2: BMP
    int theme{};
    PinStyle pin_style{DefaultPinStyle};
    // Which capture-toolbar entries stay on the panel and which live in the "更多" menu.
    ToolbarCustomization toolbar;
    UINT modifiers{MOD_CONTROL | MOD_ALT};
    UINT key{'A'};
    UINT gif_modifiers{MOD_CONTROL | MOD_ALT},gif_key{'G'};
    UINT video_modifiers{MOD_CONTROL | MOD_ALT},video_key{'R'};
    UINT clipboard_modifiers{MOD_WIN},clipboard_key{'V'};
    UINT translate_modifiers{MOD_CONTROL|MOD_ALT},translate_key{'Y'};
    UINT ocr_modifiers{MOD_CONTROL|MOD_ALT},ocr_key{'O'};
    UINT pin_modifiers{MOD_CONTROL|MOD_ALT},pin_key{'P'};
    // Selection translation: translate the text selected in any application.
    UINT selection_modifiers{MOD_CONTROL|MOD_ALT},selection_key{'T'};
    std::filesystem::path save_directory;
    // Updater: daily automatic check (after startup), last successful check in
    // Unix seconds, proxy prefixes from the last verified manifest ('|'-separated),
    // and the version that last ran (for the post-update notice).
    bool update_auto_check{true};
    long long update_last_check{};
    std::wstring update_mirrors;
    std::wstring last_run_version;
    static Preferences Load();
    void Save() const;
    static Preferences LoadFrom(const std::filesystem::path& path);
    void SaveTo(const std::filesystem::path& path) const;
    bool Dark() const;
};
bool EditPreferences(HWND owner, Preferences& preferences, const std::function<bool(const Preferences&)>& accept = {});
}