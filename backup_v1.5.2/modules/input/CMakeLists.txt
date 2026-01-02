add_library(vic_input STATIC
    src/InputInjector.cpp
    src/InputBatcher.cpp
)

configure_file(include/InputInjector.h ${CMAKE_CURRENT_BINARY_DIR}/InputInjector.h COPYONLY)
configure_file(include/InputEvents.h ${CMAKE_CURRENT_BINARY_DIR}/InputEvents.h COPYONLY)
configure_file(include/InputBatcher.h ${CMAKE_CURRENT_BINARY_DIR}/InputBatcher.h COPYONLY)

target_include_directories(vic_input
    PUBLIC
        include
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(vic_input
    PUBLIC
        user32
        vic_logging
)
