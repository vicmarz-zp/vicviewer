add_library(vic_matchmaking STATIC
    src/MatchmakerClient.cpp
)

configure_file(include/MatchmakerClient.h ${CMAKE_CURRENT_BINARY_DIR}/MatchmakerClient.h COPYONLY)

target_include_directories(vic_matchmaking
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/modules/transport/include
)

target_link_libraries(vic_matchmaking
    PUBLIC
        winhttp
        vic_logging
    vic_transport
)
