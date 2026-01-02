add_library(vic_decoder STATIC
    src/SimpleVp8Decoder.cpp
)

configure_file(include/VideoDecoder.h ${CMAKE_CURRENT_BINARY_DIR}/VideoDecoder.h COPYONLY)

# Find libyuv for optimized I420->BGRA conversion
find_package(libyuv CONFIG REQUIRED)

target_include_directories(vic_decoder
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/modules/encoder/include
        ${PROJECT_SOURCE_DIR}/modules/capture/include
)

target_link_libraries(vic_decoder
    PUBLIC
        vic_encoder
        vic_logging
    PRIVATE
        vpx::vpx
        yuv           # libyuv for SIMD I420->BGRA
)
