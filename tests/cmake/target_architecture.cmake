if(NOT DEFINED MODULE_DIR)
    message(FATAL_ERROR "MODULE_DIR is required")
endif()

list(APPEND CMAKE_MODULE_PATH "${MODULE_DIR}")
include(SaccadeTargetArchitecture)

function(expect_architecture expected system_processor osx_architectures
         vs_platform generator_platform compiler_architecture)
    saccade_resolve_target_architecture(
        actual
        "${system_processor}"
        "${osx_architectures}"
        "${vs_platform}"
        "${generator_platform}"
        "${compiler_architecture}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "expected ${expected}, got ${actual} for system=${system_processor}, "
            "osx=${osx_architectures}, vs=${vs_platform}, "
            "generator=${generator_platform}, compiler=${compiler_architecture}")
    endif()
endfunction()

expect_architecture(arm64 AMD64 "" ARM64 ARM64 "")
expect_architecture(x86_64 ARM64 "" x64 x64 "")
expect_architecture(arm64 x86_64 arm64 "" "" "")
expect_architecture(x86_64 AMD64 "" "" "" "")
expect_architecture(arm64 aarch64 "" "" "" "")
expect_architecture(arm64 AMD64 "" "" "" ARM64)
expect_architecture(x86_64 ARM64 "" "" "" x64)

function(expect_package_architecture expected system_name target_architecture)
    saccade_resolve_package_architecture(
        actual
        "${system_name}"
        "${target_architecture}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "expected package architecture ${expected}, got ${actual} for "
            "system=${system_name}, target=${target_architecture}")
    endif()
endfunction()

expect_package_architecture(arm64 Windows arm64)
expect_package_architecture(x64 Windows x86_64)
expect_package_architecture(arm64 Darwin arm64)
expect_package_architecture("" Darwin x86_64)
expect_package_architecture("" Windows riscv64)
