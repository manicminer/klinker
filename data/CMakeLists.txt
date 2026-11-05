install(FILES ${KLINKER_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES icons/${KLINKER_APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${KLINKER_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(BUILD_TESTING AND DESKTOP_FILE_VALIDATE)
    add_test(NAME desktop_file_validate
        COMMAND ${DESKTOP_FILE_VALIDATE} ${CMAKE_CURRENT_SOURCE_DIR}/${KLINKER_APP_ID}.desktop)
endif()

find_program(APPSTREAMCLI appstreamcli)
if(BUILD_TESTING AND APPSTREAMCLI)
    add_test(NAME metainfo_validate
        COMMAND ${APPSTREAMCLI} validate --no-net --explain ${CMAKE_CURRENT_SOURCE_DIR}/${KLINKER_APP_ID}.metainfo.xml)
endif()
