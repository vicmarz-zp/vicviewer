# Temporary simple transport without WebRTC dependencies
# include(FetchContent)

# set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
# set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
# set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
# set(NO_TESTS ON CACHE BOOL "" FORCE)
# set(RTC_UPDATE_VERSION_HEADER OFF CACHE BOOL "" FORCE)






include(FetchContent)

set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(RTC_UPDATE_VERSION_HEADER OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.23.2
)

FetchContent_MakeAvailable(libdatachannel)

add_library(vic_transport STATIC
    src/Transport.cpp
    src/TunnelAgent.cpp
    src/TunnelFallback.cpp
)

configure_file(include/Transport.h ${CMAKE_CURRENT_BINARY_DIR}/Transport.h COPYONLY)

set_target_properties(vic_transport PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)

target_include_directories(vic_transport
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(vic_transport
    PUBLIC
        vic_logging
        vic_input
    vic_encoder
        LibDataChannel::LibDataChannel
)

if (WIN32)
    target_link_libraries(vic_transport PUBLIC ws2_32)
endif()

