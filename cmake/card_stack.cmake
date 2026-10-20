# Clipboard card stack (docs/clipboard-card-stack.md, stage S1).
# Own file so CMakeLists.txt needs a single include() line.
target_sources(lumashot_ui PRIVATE src/clipboard/stack_window.cpp src/clipboard/card_drag.cpp src/ui/backdrop_compositor.cpp)
target_link_libraries(lumashot_ui PUBLIC runtimeobject CoreMessaging d3d11 d2d1 dwrite windowscodecs)

add_executable(lumashot_clipboard_stack_test tests/clipboard_stack_layout_test.cpp tests/clipboard_branding.rc)
target_link_libraries(lumashot_clipboard_stack_test PRIVATE lumashot_ui)
add_test(NAME clipboard_stack COMMAND lumashot_clipboard_stack_test)

# S1 part 2: the stack inside the real panel (test mode, synthetic history).
add_executable(lumashot_clipboard_stack_panel_test tests/clipboard_stack_panel_test.cpp tests/clipboard_branding.rc)
target_link_libraries(lumashot_clipboard_stack_panel_test PRIVATE lumashot_ui)
add_test(NAME clipboard_stack_panel COMMAND lumashot_clipboard_stack_panel_test)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clipboard-stack-panel")
set_tests_properties(clipboard_stack_panel PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clipboard-stack-panel" TIMEOUT 60)

# Manual probe: folded memory, classic strip vs card stack (not part of ctest).
add_executable(lumashot_clipboard_stack_memprobe EXCLUDE_FROM_ALL tests/clipboard_stack_memprobe.cpp tests/clipboard_branding.rc)
target_link_libraries(lumashot_clipboard_stack_memprobe PRIVATE lumashot_ui psapi)

# S2: card drag-out (data object, release rules, files) without DoDragDrop.
add_executable(lumashot_clipboard_card_drag_test tests/clipboard_card_drag_test.cpp tests/clipboard_branding.rc)
target_link_libraries(lumashot_clipboard_card_drag_test PRIVATE lumashot_ui)
add_test(NAME clipboard_card_drag COMMAND lumashot_clipboard_card_drag_test)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clipboard-card-drag")
set_tests_properties(clipboard_card_drag PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clipboard-card-drag" TIMEOUT 60)

# S3: entry flavours, flavour filter and the expanded panel's card model.
add_executable(lumashot_clipboard_entry_flavor_test tests/clipboard_entry_flavor_test.cpp)
target_link_libraries(lumashot_clipboard_entry_flavor_test PRIVATE lumashot_ui)
add_test(NAME clipboard_entry_flavor COMMAND lumashot_clipboard_entry_flavor_test)
set_tests_properties(clipboard_entry_flavor PROPERTIES TIMEOUT 30)
