add_library(vic_encoder STATIC
    src/SimpleVp8Encoder.cpp
    src/ColorConvert.cpp
    src/NvencEncoder.cpp
)

# Detectar si libyuv está disponible (vcpkg)
find_package(libyuv CONFIG QUIET)
if(libyuv_FOUND)
    target_compile_definitions(vic_encoder PRIVATE VIC_HAS_LIBYUV)
    target_link_libraries(vic_encoder PRIVATE yuv)
    message(STATUS "VicViewer: Using libyuv for SIMD color conversion")
else()
    message(STATUS "VicViewer: libyuv not found, using scalar color conversion")
endif()

configure_file(include/EncodedFrame.h ${CMAKE_CURRENT_BINARY_DIR}/EncodedFrame.h COPYONLY)
configure_file(include/VideoEncoder.h ${CMAKE_CURRENT_BINARY_DIR}/VideoEncoder.h COPYONLY)
configure_file(include/ColorConvert.h ${CMAKE_CURRENT_BINARY_DIR}/ColorConvert.h COPYONLY)
configure_file(include/NvencEncoder.h ${CMAKE_CURRENT_BINARY_DIR}/NvencEncoder.h COPYONLY)

target_include_directories(vic_encoder
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/modules/capture/include
)

target_link_libraries(vic_encoder
    PUBLIC
        vic_capture
        vic_logging
    PRIVATE
        vpx::vpx
)
