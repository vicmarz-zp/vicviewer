#include "ColorConvert.h"
#include "Logger.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#ifdef VIC_HAS_LIBYUV
#include <libyuv.h>
#endif

namespace vic::encoder {

namespace {

// Optimized scalar clamp
inline uint8_t clamp8(int value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

/// Scalar fallback converter - works everywhere but slower
class ScalarColorConverter final : public ColorConverter {
public:
    bool BGRAToI420(
        const uint8_t* src_bgra, int src_stride_bgra,
        uint8_t* dst_y, int dst_stride_y,
        uint8_t* dst_u, int dst_stride_u,
        uint8_t* dst_v, int dst_stride_v,
        int width, int height) override 
    {
        if (!src_bgra || !dst_y || !dst_u || !dst_v || width <= 0 || height <= 0) {
            return false;
        }

        // Process Y plane - full resolution
        for (int y = 0; y < height; ++y) {
            const uint8_t* src_row = src_bgra + y * src_stride_bgra;
            uint8_t* y_row = dst_y + y * dst_stride_y;
            
            for (int x = 0; x < width; ++x) {
                const uint8_t b = src_row[x * 4 + 0];
                const uint8_t g = src_row[x * 4 + 1];
                const uint8_t r = src_row[x * 4 + 2];
                
                // BT.601 conversion
                y_row[x] = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
        }

        // Process U and V planes - half resolution with 2x2 averaging
        const int uv_width = (width + 1) / 2;
        const int uv_height = (height + 1) / 2;

        for (int y = 0; y < uv_height; ++y) {
            const int src_y0 = y * 2;
            const int src_y1 = std::min(src_y0 + 1, height - 1);
            
            const uint8_t* src_row0 = src_bgra + src_y0 * src_stride_bgra;
            const uint8_t* src_row1 = src_bgra + src_y1 * src_stride_bgra;
            
            uint8_t* u_row = dst_u + y * dst_stride_u;
            uint8_t* v_row = dst_v + y * dst_stride_v;

            for (int x = 0; x < uv_width; ++x) {
                const int src_x0 = x * 2;
                const int src_x1 = std::min(src_x0 + 1, width - 1);

                // Sample 4 pixels (2x2 block)
                int r_sum = 0, g_sum = 0, b_sum = 0;
                
                // Top-left
                b_sum += src_row0[src_x0 * 4 + 0];
                g_sum += src_row0[src_x0 * 4 + 1];
                r_sum += src_row0[src_x0 * 4 + 2];
                
                // Top-right
                b_sum += src_row0[src_x1 * 4 + 0];
                g_sum += src_row0[src_x1 * 4 + 1];
                r_sum += src_row0[src_x1 * 4 + 2];
                
                // Bottom-left
                b_sum += src_row1[src_x0 * 4 + 0];
                g_sum += src_row1[src_x0 * 4 + 1];
                r_sum += src_row1[src_x0 * 4 + 2];
                
                // Bottom-right
                b_sum += src_row1[src_x1 * 4 + 0];
                g_sum += src_row1[src_x1 * 4 + 1];
                r_sum += src_row1[src_x1 * 4 + 2];

                // Average
                const int r = r_sum / 4;
                const int g = g_sum / 4;
                const int b = b_sum / 4;

                // BT.601 conversion for UV
                u_row[x] = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                v_row[x] = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }

        return true;
    }

    bool I420ToBGRA(
        const uint8_t* src_y, int src_stride_y,
        const uint8_t* src_u, int src_stride_u,
        const uint8_t* src_v, int src_stride_v,
        uint8_t* dst_bgra, int dst_stride_bgra,
        int width, int height) override 
    {
        if (!src_y || !src_u || !src_v || !dst_bgra || width <= 0 || height <= 0) {
            return false;
        }

        for (int y = 0; y < height; ++y) {
            const uint8_t* y_row = src_y + y * src_stride_y;
            const uint8_t* u_row = src_u + (y / 2) * src_stride_u;
            const uint8_t* v_row = src_v + (y / 2) * src_stride_v;
            uint8_t* dst_row = dst_bgra + y * dst_stride_bgra;

            for (int x = 0; x < width; ++x) {
                const int Y = static_cast<int>(y_row[x]) - 16;
                const int U = static_cast<int>(u_row[x / 2]) - 128;
                const int V = static_cast<int>(v_row[x / 2]) - 128;

                // BT.601 YUV to RGB
                const int c = 298 * Y;
                dst_row[x * 4 + 2] = clamp8((c + 409 * V + 128) >> 8);           // R
                dst_row[x * 4 + 1] = clamp8((c - 100 * U - 208 * V + 128) >> 8); // G
                dst_row[x * 4 + 0] = clamp8((c + 516 * U + 128) >> 8);           // B
                dst_row[x * 4 + 3] = 255;                                         // A
            }
        }

        return true;
    }

    const char* name() const override { return "Scalar"; }
};

#ifdef VIC_HAS_LIBYUV
/// libyuv converter - SIMD optimized (SSE2/AVX2/NEON)
class LibyuvColorConverter final : public ColorConverter {
public:
    bool BGRAToI420(
        const uint8_t* src_bgra, int src_stride_bgra,
        uint8_t* dst_y, int dst_stride_y,
        uint8_t* dst_u, int dst_stride_u,
        uint8_t* dst_v, int dst_stride_v,
        int width, int height) override 
    {
        // NOTA: En libyuv, "ARGB" = bytes [B,G,R,A] en memoria = Windows "BGRA"
        // Por eso ARGBToI420 es correcto para datos BGRA de Windows/DXGI
        return libyuv::ARGBToI420(
            src_bgra, src_stride_bgra,
            dst_y, dst_stride_y,
            dst_u, dst_stride_u,
            dst_v, dst_stride_v,
            width, height) == 0;
    }

    bool I420ToBGRA(
        const uint8_t* src_y, int src_stride_y,
        const uint8_t* src_u, int src_stride_u,
        const uint8_t* src_v, int src_stride_v,
        uint8_t* dst_bgra, int dst_stride_bgra,
        int width, int height) override 
    {
        // NOTA: En libyuv, "ARGB" = bytes [B,G,R,A] en memoria = Windows "BGRA"
        // Por eso I420ToARGB produce el formato correcto para Windows
        return libyuv::I420ToARGB(
            src_y, src_stride_y,
            src_u, src_stride_u,
            src_v, src_stride_v,
            dst_bgra, dst_stride_bgra,
            width, height) == 0;
    }

    const char* name() const override { return "libyuv (SIMD)"; }
};

std::unique_ptr<ColorConverter> createLibyuvColorConverter() {
    return std::make_unique<LibyuvColorConverter>();
}
#endif

} // anonymous namespace

std::unique_ptr<ColorConverter> createScalarColorConverter() {
    return std::make_unique<ScalarColorConverter>();
}

std::unique_ptr<ColorConverter> createColorConverter() {
#ifdef VIC_HAS_LIBYUV
    logging::global().log(logging::Logger::Level::Info, "Using libyuv SIMD color converter");
    return createLibyuvColorConverter();
#else
    logging::global().log(logging::Logger::Level::Info, "Using scalar color converter (libyuv not available)");
    return createScalarColorConverter();
#endif
}

} // namespace vic::encoder
