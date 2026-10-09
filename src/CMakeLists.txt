add_library(kbs_core STATIC
    core/rules.cpp
    core/urlcleaner.cpp
)

target_include_directories(kbs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kbs_core PUBLIC Qt6::Core PRIVATE KF6::ConfigCore)
target_compile_definitions(kbs_core PUBLIC QT_NO_CAST_FROM_ASCII QT_NO_URL_CAST_FROM_STRING)
target_compile_options(kbs_core PRIVATE -Wall -Wextra -Wpedantic)
