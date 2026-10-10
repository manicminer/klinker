cmake_minimum_required(VERSION 3.22)

project(klinker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# Desktop file ID (without ".desktop"), also used as the icon name.
set(KBS_APP_ID "io.github.manicminer.Klinker")

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network DBus)
find_package(KF6Config 6.0 REQUIRED)
# 6.10: KBusyIndicatorWidget::start() and stop()
find_package(KF6WidgetsAddons 6.10 REQUIRED)
# 6.3: KIconTheme::initTheme()
find_package(KF6IconThemes 6.3 REQUIRED)

# Required: on Wayland, layer shell is the only way the chooser can be centred
# on the active output (a regular client cannot position its own window).
# setScreen() needs 6.6.
find_package(LayerShellQt 6.6 REQUIRED)

include(CTest)

add_subdirectory(src)
add_subdirectory(data)

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    # QWindowSystemInterface, for tests that report focus changes as the platform would.
    find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS GuiPrivate)
    find_package(KF6ColorScheme 6.0 REQUIRED)
    add_subdirectory(tests)
endif()
