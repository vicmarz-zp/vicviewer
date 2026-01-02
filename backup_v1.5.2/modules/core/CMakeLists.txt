add_library(vic_core STATIC
    src/AppContext.cpp
    src/Metrics.cpp
)

target_include_directories(vic_core PUBLIC include)

target_link_libraries(vic_core PUBLIC
    ws2_32
)
