function(klinker_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE klinker_core Qt6::Test)
    target_compile_definitions(${name} PRIVATE KLINKER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

klinker_add_test(test_urlcleaner)
klinker_add_test(test_rules)
klinker_add_test(test_execcommand)
klinker_add_test(test_browserdiscovery)
klinker_add_test(test_browsercatalog)
klinker_add_test(test_config)

klinker_add_test(test_redirectresolver)
target_link_libraries(test_redirectresolver PRIVATE klinker_network)

# Records its argv as a JSON array so launch tests never start a real browser.
add_executable(klinker_capture_fixture fixtures/capture.cpp)
target_link_libraries(klinker_capture_fixture PRIVATE Qt6::Core)

# A second application that takes the focus, only started by Wayland tests
# inside the private nested KWin.
add_executable(klinker_focus_fixture fixtures/focusfixture.cpp)
target_link_libraries(klinker_focus_fixture PRIVATE Qt6::Widgets)

# GUI tests run offscreen by default. Run them with QT_QPA_PLATFORM=wayland
# (see tests/wayland-smoke.sh) to exercise layer-shell placement for real, and
# set KLINKER_SCREENSHOT_DIR to keep the screenshots they take.
function(klinker_add_gui_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE klinker_app Qt6::Test Qt6::GuiPrivate KF6::IconThemes KF6::ColorScheme KF6::ConfigCore)
    target_compile_definitions(${name} PRIVATE
        KLINKER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        KLINKER_CAPTURE_FIXTURE="$<TARGET_FILE:klinker_capture_fixture>"
        KLINKER_FOCUS_FIXTURE="$<TARGET_FILE:klinker_focus_fixture>"
        KLINKER_APP_EXECUTABLE="$<TARGET_FILE:klinker>")
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(${name} klinker_capture_fixture klinker_focus_fixture klinker)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

klinker_add_gui_test(test_placement)
klinker_add_gui_test(test_chooser testsupport.cpp)
klinker_add_gui_test(test_settings testsupport.cpp)
klinker_add_gui_test(test_application testsupport.cpp)

if(LayerShellQt_FOUND)
    target_link_libraries(test_chooser PRIVATE LayerShellQt::Interface)
    target_compile_definitions(test_chooser PRIVATE KLINKER_HAVE_LAYERSHELLQT)
endif()

# Opt-in: runs the Wayland-only test functions in a private, virtual KWin.
option(KLINKER_WAYLAND_TESTS "Run Wayland smoke tests in a nested virtual kwin_wayland" OFF)
if(KLINKER_WAYLAND_TESTS)
    add_test(NAME wayland_smoke
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/wayland-smoke.sh $<TARGET_FILE:test_chooser>
            waylandLayerShellPopup waylandFocusLoss waylandOwnPopupKeepsTheChooser)
    set_tests_properties(wayland_smoke PROPERTIES SKIP_RETURN_CODE 77)
    # The chooser as the application shows it, sized for the virtual output.
    add_test(NAME wayland_smoke_controller
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/wayland-smoke.sh $<TARGET_FILE:test_application>
            horizontalChooserFitsItsBrowsers)
    set_tests_properties(wayland_smoke_controller PROPERTIES SKIP_RETURN_CODE 77)
endif()
