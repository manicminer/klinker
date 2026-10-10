function(kbs_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE kbs_core Qt6::Test)
    target_compile_definitions(${name} PRIVATE KBS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

kbs_add_test(test_urlcleaner)
kbs_add_test(test_rules)
kbs_add_test(test_execcommand)
kbs_add_test(test_browserdiscovery)
kbs_add_test(test_browsercatalog)
kbs_add_test(test_config)

kbs_add_test(test_redirectresolver)
target_link_libraries(test_redirectresolver PRIVATE kbs_network)

# Records its argv as a JSON array so launch tests never start a real browser.
add_executable(kbs_capture_fixture fixtures/capture.cpp)
target_link_libraries(kbs_capture_fixture PRIVATE Qt6::Core)

# GUI tests run offscreen by default. Run them with QT_QPA_PLATFORM=wayland
# (see tests/wayland-smoke.sh) to exercise layer-shell placement for real, and
# set KBS_SCREENSHOT_DIR to keep the screenshots they take.
function(kbs_add_gui_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE kbs_ui Qt6::Test KF6::IconThemes KF6::ColorScheme KF6::ConfigCore)
    target_compile_definitions(${name} PRIVATE
        KBS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        KBS_CAPTURE_FIXTURE="$<TARGET_FILE:kbs_capture_fixture>")
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(${name} kbs_capture_fixture)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

kbs_add_gui_test(test_placement)
kbs_add_gui_test(test_chooser testsupport.cpp)
kbs_add_gui_test(test_settings testsupport.cpp)

if(LayerShellQt_FOUND)
    target_link_libraries(test_chooser PRIVATE LayerShellQt::Interface)
    target_compile_definitions(test_chooser PRIVATE KBS_HAVE_LAYERSHELLQT)
endif()

# Opt-in: runs the Wayland-only test functions in a private, virtual KWin.
option(KBS_WAYLAND_TESTS "Run Wayland smoke tests in a nested virtual kwin_wayland" OFF)
if(KBS_WAYLAND_TESTS)
    add_test(NAME wayland_smoke
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/wayland-smoke.sh $<TARGET_FILE:test_chooser>)
    set_tests_properties(wayland_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()
