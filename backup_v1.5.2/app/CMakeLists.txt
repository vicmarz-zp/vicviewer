add_executable(VicViewer
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../resources/app.rc
)

target_link_libraries(VicViewer
    PRIVATE
        vic_ui
        vic_core
)

set_target_properties(VicViewer PROPERTIES
    WIN32_EXECUTABLE YES
)

# Optimizaciones para Release - Anti ingeniería inversa
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
    # Optimización máxima y quitar símbolos de debug
    target_compile_options(VicViewer PRIVATE
        $<$<CONFIG:Release>:/O2>           # Optimización máxima
        $<$<CONFIG:Release>:/Ob2>          # Inline agresivo
        $<$<CONFIG:Release>:/Oi>           # Intrinsics
        $<$<CONFIG:Release>:/Ot>           # Favorecer velocidad
        $<$<CONFIG:Release>:/GL>           # Whole program optimization
        $<$<CONFIG:Release>:/GS->          # Deshabilitar buffer security check (más rápido)
        $<$<CONFIG:Release>:/Gy>           # Function-level linking
    )
    
    target_link_options(VicViewer PRIVATE
        $<$<CONFIG:Release>:/LTCG>         # Link time code generation
        $<$<CONFIG:Release>:/OPT:REF>      # Eliminar funciones no referenciadas
        $<$<CONFIG:Release>:/OPT:ICF>      # Combinar funciones idénticas
        $<$<CONFIG:Release>:/INCREMENTAL:NO>  # No incremental linking
    )
endif()

# Auto-empaquetado en build Release: copia ejecutable y dependencias a dist
add_custom_command(TARGET VicViewer POST_BUILD
    COMMAND 
        "$<$<CONFIG:Release>:powershell.exe>" -NoProfile -ExecutionPolicy Bypass -File "C:/vic_viewer/scripts/prepare_dist.ps1" -Configuration "$<CONFIG>" -SkipBuild
    COMMENT "Empaquetando (prepare_dist.ps1) para configuración $<CONFIG>"
    VERBATIM
)
