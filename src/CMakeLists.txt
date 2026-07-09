add_library(saccade_core STATIC core/version.cpp)
add_library(Saccade::core ALIAS saccade_core)

target_include_directories(saccade_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(saccade_core PRIVATE saccade_build_settings)
