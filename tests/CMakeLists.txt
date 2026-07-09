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
