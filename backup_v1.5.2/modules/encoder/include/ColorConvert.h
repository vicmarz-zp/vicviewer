#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

namespace vic::encoder {

/// Interface for color conversion operations.
/// Implementations can use SIMD (libyuv), GPU shaders, or fallback to scalar code.
class ColorConverter {
public:
    virtual ~ColorConverter() = default;

    /// Convert BGRA to I420 (YUV420 planar).
    /// @param src_bgra Source BGRA data (4 bytes per pixel)
    /// @param src_stride_bgra Stride in bytes (usually width * 4)
    /// @param dst_y Destination Y plane
    /// @param dst_stride_y Y plane stride
    /// @param dst_u Destination U plane
    /// @param dst_stride_u U plane stride
    /// @param dst_v Destination V plane
    /// @param dst_stride_v V plane stride
    /// @param width Image width
    /// @param height Image height
    /// @return true on success
    virtual bool BGRAToI420(
        const uint8_t* src_bgra, int src_stride_bgra,
        uint8_t* dst_y, int dst_stride_y,
        uint8_t* dst_u, int dst_stride_u,
        uint8_t* dst_v, int dst_stride_v,
        int width, int height) = 0;

    /// Convert I420 to BGRA.
    virtual bool I420ToBGRA(
        const uint8_t* src_y, int src_stride_y,
        const uint8_t* src_u, int src_stride_u,
        const uint8_t* src_v, int src_stride_v,
        uint8_t* dst_bgra, int dst_stride_bgra,
        int width, int height) = 0;

    /// Get converter name for logging
    virtual const char* name() const = 0;
};

/// Create the best available color converter.
/// Tries libyuv first, falls back to optimized scalar if not available.
std::unique_ptr<ColorConverter> createColorConverter();

/// Fallback scalar converter (always available)
std::unique_ptr<ColorConverter> createScalarColorConverter();

} // namespace vic::encoder
