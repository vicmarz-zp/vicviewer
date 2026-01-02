add_library(vic_ui STATIC
    src/VicViewerUI.cpp
    src/FrameRenderer.cpp
    src/AntiAbuse.cpp
)

configure_file(include/VicViewerUI.h ${CMAKE_CURRENT_BINARY_DIR}/VicViewerUI.h COPYONLY)
configure_file(include/FrameRenderer.h ${CMAKE_CURRENT_BINARY_DIR}/FrameRenderer.h COPYONLY)
configure_file(include/vic/ui/AntiAbuse.h ${CMAKE_CURRENT_BINARY_DIR}/vic/ui/AntiAbuse.h COPYONLY)

target_include_directories(vic_ui
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/modules/core/include
        ${PROJECT_SOURCE_DIR}/modules/pipeline/include
        ${PROJECT_SOURCE_DIR}/modules/matchmaking/include
        ${PROJECT_SOURCE_DIR}/modules/capture/include
)

target_link_libraries(vic_ui
    PUBLIC
        vic_pipeline
        vic_matchmaking
        vic_logging
        vic_core
        comctl32
        shell32
        user32
        gdi32
        d3d11
        d3dcompiler
        dxgi
        iphlpapi
)
