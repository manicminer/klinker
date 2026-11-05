add_library(klinker_core STATIC
    core/browsercatalog.cpp
    core/browserdiscovery.cpp
    core/config.cpp
    core/execcommand.cpp
    core/hostsystem.cpp
    core/rules.cpp
    core/urlcleaner.cpp
)

target_include_directories(klinker_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(klinker_core PUBLIC Qt6::Core PRIVATE KF6::ConfigCore)
target_compile_definitions(klinker_core PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_URL_CAST_FROM_STRING KLINKER_APP_ID="${KLINKER_APP_ID}")
target_compile_options(klinker_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(klinker_network STATIC
    network/redirectresolver.cpp
)

target_link_libraries(klinker_network PUBLIC klinker_core Qt6::Network)
target_compile_options(klinker_network PRIVATE -Wall -Wextra -Wpedantic)

# Widgets: chooser, settings and window placement.
add_library(klinker_ui STATIC
    ui/browsereditdialog.cpp ui/browsereditdialog.h
    ui/browserlistmodel.cpp ui/browserlistmodel.h
    ui/chooserwindow.cpp ui/chooserwindow.h
    ui/documentportal.cpp ui/documentportal.h
    ui/placement.cpp ui/placement.h
    ui/settingsdialog.cpp ui/settingsdialog.h
    ui/theme.cpp ui/theme.h
    ui/resources.qrc
)

target_link_libraries(klinker_ui
    PUBLIC klinker_core Qt6::Widgets KF6::WidgetsAddons
    PRIVATE Qt6::DBus KF6::IconWidgets
)
target_compile_definitions(klinker_ui PUBLIC KLINKER_VERSION="${PROJECT_VERSION}")
target_compile_options(klinker_ui PRIVATE -Wall -Wextra -Wpedantic)
if(LayerShellQt_FOUND)
    target_link_libraries(klinker_ui PRIVATE LayerShellQt::Interface)
    target_compile_definitions(klinker_ui PRIVATE KLINKER_HAVE_LAYERSHELLQT)
endif()

# Command line, URL pipeline, launching and the window flow between them.
add_library(klinker_app STATIC
    app/application.cpp app/application.h
    app/commandline.cpp app/commandline.h
    app/launcher.cpp app/launcher.h
    app/linkpipeline.cpp app/linkpipeline.h
)

target_link_libraries(klinker_app PUBLIC klinker_ui klinker_network Qt6::Widgets PRIVATE Qt6::DBus)
target_compile_options(klinker_app PRIVATE -Wall -Wextra -Wpedantic)

add_executable(klinker main.cpp)
target_link_libraries(klinker PRIVATE klinker_app KF6::IconThemes)
target_compile_options(klinker PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS klinker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
