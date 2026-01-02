cmake_minimum_required(VERSION 3.20)

add_executable(VicViewerService WIN32
    main.cpp
)

# Cambiar de WIN32 a consola para poder ver output de --help
set_target_properties(VicViewerService PROPERTIES
    WIN32_EXECUTABLE FALSE
)

target_link_libraries(VicViewerService PRIVATE
    vic_pipeline
    vic_logging
    Advapi32
)

target_include_directories(VicViewerService PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/pipeline/include
    ${CMAKE_SOURCE_DIR}/modules/logging/include
)

# Copiar DLLs necesarios
add_custom_command(TARGET VicViewerService POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:datachannel>
        $<TARGET_FILE_DIR:VicViewerService>
)
