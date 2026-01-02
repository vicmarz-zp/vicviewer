add_library(vic_pipeline STATIC
    src/HostSession.cpp
    src/ViewerSession.cpp
)

configure_file(include/HostSession.h ${CMAKE_CURRENT_BINARY_DIR}/HostSession.h COPYONLY)
configure_file(include/ViewerSession.h ${CMAKE_CURRENT_BINARY_DIR}/ViewerSession.h COPYONLY)
configure_file(include/StreamConfig.h ${CMAKE_CURRENT_BINARY_DIR}/StreamConfig.h COPYONLY)

target_include_directories(vic_pipeline
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/modules/capture/include
        ${PROJECT_SOURCE_DIR}/modules/encoder/include
        ${PROJECT_SOURCE_DIR}/modules/decoder/include
        ${PROJECT_SOURCE_DIR}/modules/transport/include
        ${PROJECT_SOURCE_DIR}/modules/input/include
        ${PROJECT_SOURCE_DIR}/modules/logging/include
        ${PROJECT_SOURCE_DIR}/modules/matchmaking/include
)

target_link_libraries(vic_pipeline
    PUBLIC
        vic_capture
        vic_encoder
        vic_decoder
        vic_transport
        vic_input
        vic_logging
        vic_matchmaking
)
