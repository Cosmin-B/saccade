add_executable(saccade_build_smoke smoke/build_smoke.cpp)
target_link_libraries(saccade_build_smoke PRIVATE saccade_build_settings)
add_test(NAME smoke.build COMMAND saccade_build_smoke)

add_executable(saccade_header_c abi/header_c.c)
target_include_directories(saccade_header_c PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_header_c PRIVATE saccade_core saccade_build_settings)
add_test(NAME abi.header_c COMMAND saccade_header_c)

add_executable(saccade_header_cpp abi/header_cpp.cpp)
target_include_directories(saccade_header_cpp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_header_cpp PRIVATE saccade_core saccade_build_settings)
add_test(NAME abi.header_cpp COMMAND saccade_header_cpp)

add_executable(saccade_abi_layout abi/layout.c)
target_include_directories(saccade_abi_layout PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_abi_layout PRIVATE saccade_build_settings)
add_test(NAME abi.layout COMMAND saccade_abi_layout)

add_executable(saccade_typed_facade_c abi/typed_facade_c.c)
target_include_directories(saccade_typed_facade_c PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_typed_facade_c PRIVATE saccade_build_settings)
add_test(NAME abi.typed_facade_c COMMAND saccade_typed_facade_c)

add_executable(saccade_typed_facade_cpp abi/typed_facade_cpp.cpp)
target_include_directories(saccade_typed_facade_cpp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_typed_facade_cpp PRIVATE saccade_build_settings)
add_test(NAME abi.typed_facade_cpp COMMAND saccade_typed_facade_cpp)

set(typed_facade_script "${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_reject.cmake")
set(typed_facade_common_arguments
    "-DC_COMPILER=${CMAKE_C_COMPILER}"
    "-DC_COMPILER_ID=${CMAKE_C_COMPILER_ID}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
    "-DINCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
    "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}")

add_test(
    NAME abi.typed_facade.reject
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=reject
        ${typed_facade_common_arguments}
        "-DUNSUPPORTED_C_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_unsupported.c"
        "-DUNSUPPORTED_CXX_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_unsupported.cpp"
        -P "${typed_facade_script}")

add_test(
    NAME abi.typed_facade.static_dispatch
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=preprocess
        ${typed_facade_common_arguments}
        "-DSUPPORTED_C_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_c.c"
        "-DSUPPORTED_CXX_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_cpp.cpp"
        -P "${typed_facade_script}")

add_executable(saccade_backend_header_c abi/backend_header_c.c)
target_include_directories(saccade_backend_header_c PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_backend_header_c PRIVATE saccade_build_settings)
add_test(NAME abi.backend.header_c COMMAND saccade_backend_header_c)

add_executable(saccade_backend_header_cpp abi/backend_header_cpp.cpp)
target_include_directories(saccade_backend_header_cpp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_backend_header_cpp PRIVATE saccade_build_settings)
add_test(NAME abi.backend.header_cpp COMMAND saccade_backend_header_cpp)

add_executable(saccade_backend_layout abi/backend_layout.c)
target_include_directories(saccade_backend_layout PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_backend_layout PRIVATE saccade_build_settings)
add_test(NAME abi.backend.layout COMMAND saccade_backend_layout)
