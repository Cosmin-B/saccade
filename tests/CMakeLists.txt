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

add_executable(saccade_abi_manifest abi/manifest.c)
target_include_directories(saccade_abi_manifest PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_abi_manifest PRIVATE saccade_build_settings)
add_test(NAME abi.manifest.layout COMMAND saccade_abi_manifest)

add_test(
    NAME abi.manifest.symbols
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/symbol_manifest.cmake")

add_executable(saccade_core_error core/error.cpp)
target_include_directories(saccade_core_error PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_error PRIVATE
    saccade_core
    saccade_build_settings
    Threads::Threads)
add_test(NAME core.error COMMAND saccade_core_error)

add_executable(saccade_core_handle_table core/handle_table.cpp)
target_include_directories(saccade_core_handle_table PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_handle_table PRIVATE saccade_build_settings)
add_test(NAME core.handle_table COMMAND saccade_core_handle_table)

add_executable(saccade_core_frame_lease core/frame_lease.cpp)
target_include_directories(saccade_core_frame_lease PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_frame_lease PRIVATE saccade_build_settings)
add_test(NAME core.frame_lease COMMAND saccade_core_frame_lease)

add_executable(saccade_core_newest_frame_mailbox core/newest_frame_mailbox.cpp)
target_include_directories(saccade_core_newest_frame_mailbox PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_newest_frame_mailbox PRIVATE
    saccade_build_settings
    Threads::Threads)
add_test(NAME core.newest_frame_mailbox COMMAND saccade_core_newest_frame_mailbox)

add_executable(saccade_backend_registry backend/registry.cpp)
target_include_directories(saccade_backend_registry PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_registry PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME backend.registry COMMAND saccade_backend_registry)

add_executable(saccade_backend_device_registry backend/device_registry.cpp)
target_include_directories(saccade_backend_device_registry PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_device_registry PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME backend.device_registry COMMAND saccade_backend_device_registry)

if(TARGET saccade_backend_mock)
    add_executable(saccade_backend_mock_contract backend/mock_contract.cpp)
    target_include_directories(saccade_backend_mock_contract PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_backend_mock_contract PRIVATE
        saccade_backend_mock
        saccade_core
        saccade_build_settings)
    add_test(NAME backend.mock_contract COMMAND saccade_backend_mock_contract)
endif()

if(TARGET saccade_backend_reference_cpu)
    add_executable(saccade_backend_reference_cpu_contract
        backend/reference_cpu_contract.cpp)
    target_include_directories(saccade_backend_reference_cpu_contract PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_backend_reference_cpu_contract PRIVATE
        saccade_backend_reference_cpu
        saccade_core
        saccade_build_settings)
    add_test(
        NAME backend.reference_cpu_contract
        COMMAND saccade_backend_reference_cpu_contract)
endif()

add_executable(saccade_backend_registry_guard backend/registry_guard.cpp)
target_include_directories(saccade_backend_registry_guard PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_registry_guard PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME backend.registry_guard COMMAND saccade_backend_registry_guard)

add_executable(saccade_core_runtime core/runtime.cpp)
target_include_directories(saccade_core_runtime PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_core_runtime PRIVATE saccade_core saccade_build_settings)
add_test(NAME core.runtime COMMAND saccade_core_runtime)

add_executable(saccade_core_runtime_static_destruction
    core/runtime_static_destruction.cpp)
target_include_directories(saccade_core_runtime_static_destruction PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_core_runtime_static_destruction PRIVATE
    saccade_core
    saccade_build_settings)
add_test(
    NAME core.runtime_static_destruction
    COMMAND saccade_core_runtime_static_destruction)

add_test(
    NAME package.shared_build
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/shared-build"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DC_COMPILER=${CMAKE_C_COMPILER}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DNM_TOOL=${CMAKE_NM}"
        "-DAPPLE_HOST=${APPLE}"
        "-DWIN32_HOST=${WIN32}"
        "-DSYMBOL_MANIFEST=${PROJECT_SOURCE_DIR}/abi/symbols-v1.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shared_build.cmake")

set(install_consumer_arguments
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/install-consumer"
    "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/install-consumer"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DC_COMPILER=${CMAKE_C_COMPILER}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}")

add_test(
    NAME package.install.static
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=static
        ${install_consumer_arguments}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_consumer.cmake")

add_test(
    NAME package.install.shared
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=shared
        ${install_consumer_arguments}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_consumer.cmake")

add_test(
    NAME package.install.both
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=both
        ${install_consumer_arguments}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_consumer.cmake")

add_test(
    NAME quality.repository
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/scripts/check_repository.cmake")

add_test(
    NAME quality.text
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/scripts/check_text.cmake")

add_test(
    NAME quality.documentation
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/scripts/check_documentation.cmake")
