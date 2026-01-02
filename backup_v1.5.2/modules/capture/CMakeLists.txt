add_library(vic_capture STATIC
    src/DesktopCapturer.cpp
    src/GdiCapturer.cpp
    src/FrameScaler.cpp
)

configure_file(include/DesktopFrame.h ${CMAKE_CURRENT_BINARY_DIR}/DesktopFrame.h COPYONLY)
configure_file(include/FrameScaler.h ${CMAKE_CURRENT_BINARY_DIR}/FrameScaler.h COPYONLY)
configure_file(include/DesktopCapturer.h ${CMAKE_CURRENT_BINARY_DIR}/DesktopCapturer.h COPYONLY)

# Find libyuv for optimized scaling
find_package(libyuv CONFIG REQUIRED)

target_include_directories(vic_capture
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(vic_capture
    PUBLIC
        vic_logging
        gdi32
        d3d11
        dxgi
    PRIVATE
        yuv  # libyuv for SIMD scaling
)
