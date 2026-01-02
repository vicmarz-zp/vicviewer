enable_testing()

add_executable(vic_unit_tests
    VideoPipelineTests.cpp
)

target_link_libraries(vic_unit_tests
    PRIVATE
        vic_encoder
        vic_decoder
        vic_capture
)

add_test(NAME EncodeDecodeRoundtrip COMMAND vic_unit_tests)

add_executable(vic_e2e_test
    EndToEndHostViewerTest.cpp
)

target_link_libraries(vic_e2e_test
    PRIVATE
        vic_pipeline
        vic_matchmaking
        vic_transport
        vic_encoder
        vic_decoder
        vic_capture
        vic_input
        vic_logging
)

add_test(NAME HostViewerEndToEnd COMMAND vic_e2e_test)

add_executable(vic_benchmark
    benchmark_perf.cpp
)

target_link_libraries(vic_benchmark
    PRIVATE
        vic_encoder
        vic_decoder
        vic_capture
)

add_executable(vic_nvenc_test
    test_nvenc.cpp
)

target_link_libraries(vic_nvenc_test
    PRIVATE
        vic_encoder
        vic_capture
)

add_executable(vic_multirez_bench
    benchmark_multirez.cpp
)

target_link_libraries(vic_multirez_bench
    PRIVATE
        vic_encoder
        vic_decoder
        vic_capture
)

# Benchmark decoder optimizado (libyuv SIMD + buffer reuse)
add_executable(vic_decoder_bench
    benchmark_decoder.cpp
)

target_link_libraries(vic_decoder_bench
    PRIVATE
        vic_encoder
        vic_decoder
        vic_capture
)
