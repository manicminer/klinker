add_library(kbs_core STATIC
    core/browsercatalog.cpp
    core/browserdiscovery.cpp
    core/config.cpp
    core/execcommand.cpp
    core/rules.cpp
    core/urlcleaner.cpp
)

target_include_directories(kbs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kbs_core PUBLIC Qt6::Core PRIVATE KF6::ConfigCore)
target_compile_definitions(kbs_core PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_URL_CAST_FROM_STRING)
target_compile_options(kbs_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(kbs_network STATIC
    network/redirectresolver.cpp
)

target_link_libraries(kbs_network PUBLIC kbs_core Qt6::Network)
target_compile_options(kbs_network PRIVATE -Wall -Wextra -Wpedantic)

# Widgets: chooser, settings and window placement.
add_library(kbs_ui STATIC
    ui/browsereditdialog.cpp ui/browsereditdialog.h
    ui/browserlistmodel.cpp ui/browserlistmodel.h
    ui/chooserwindow.cpp ui/chooserwindow.h
    ui/placement.cpp ui/placement.h
    ui/settingsdialog.cpp ui/settingsdialog.h
    ui/theme.cpp ui/theme.h
    ui/resources.qrc
)

target_link_libraries(kbs_ui
    PUBLIC kbs_core Qt6::Widgets KF6::WidgetsAddons
    PRIVATE Qt6::DBus KF6::IconWidgets
)
target_compile_definitions(kbs_ui PUBLIC KBS_APP_ID="${KBS_APP_ID}" KBS_VERSION="${PROJECT_VERSION}")
target_compile_options(kbs_ui PRIVATE -Wall -Wextra -Wpedantic)
if(LayerShellQt_FOUND)
    target_link_libraries(kbs_ui PRIVATE LayerShellQt::Interface)
    target_compile_definitions(kbs_ui PRIVATE KBS_HAVE_LAYERSHELLQT)
endif()

# Command line, URL pipeline, launching and the window flow between them.
add_library(kbs_app STATIC
    app/application.cpp app/application.h
    app/commandline.cpp app/commandline.h
    app/launcher.cpp app/launcher.h
    app/linkpipeline.cpp app/linkpipeline.h
)

target_link_libraries(kbs_app PUBLIC kbs_ui kbs_network Qt6::Widgets)
target_compile_options(kbs_app PRIVATE -Wall -Wextra -Wpedantic)

add_executable(klinker main.cpp)
target_link_libraries(klinker PRIVATE kbs_app KF6::IconThemes)
target_compile_options(klinker PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS klinker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
