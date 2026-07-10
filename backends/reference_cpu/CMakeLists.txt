add_library(saccade_backend_reference_cpu STATIC reference_cpu.cpp)
target_include_directories(saccade_backend_reference_cpu
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_reference_cpu
    PUBLIC Threads::Threads
    PRIVATE saccade_build_settings)
add_library(Saccade::reference_cpu ALIAS saccade_backend_reference_cpu)
