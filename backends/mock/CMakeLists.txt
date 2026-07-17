add_library(saccade_backend_mock STATIC mock_backend.cpp)
target_include_directories(saccade_backend_mock
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_mock
    PUBLIC Threads::Threads
    PRIVATE saccade_core saccade_build_settings)
add_library(Saccade::mock ALIAS saccade_backend_mock)
