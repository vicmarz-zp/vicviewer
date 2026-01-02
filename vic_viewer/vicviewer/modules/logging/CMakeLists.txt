add_library(vic_logging STATIC
    src/Logger.cpp
)

target_include_directories(vic_logging PUBLIC include)

target_link_libraries(vic_logging PUBLIC
    vic_core
)
