option(SACCADE_BUILD_STATIC "Build static libraries" ON)
option(SACCADE_BUILD_SHARED "Build shared libraries" OFF)
option(SACCADE_BUILD_TESTS "Build tests" ON)
option(SACCADE_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(SACCADE_BUILD_TOOLS "Build tools" ON)
option(SACCADE_BUILD_APPLICATION "Build the native desktop application" ${SACCADE_BUILD_STATIC})
option(SACCADE_DISTRIBUTION_BUILD
    "Require complete signed and notarized desktop release inputs" OFF)
option(SACCADE_BACKEND_MOCK "Build deterministic contract-test providers" ON)
option(SACCADE_BACKEND_REFERENCE_CPU "Build scalar reference CPU providers" ON)
option(SACCADE_BACKEND_METAL "Build the native Metal backend" ${APPLE})
option(SACCADE_BACKEND_D3D12 "Build the native Direct3D 12 backend" ${WIN32})
option(SACCADE_ENABLE_TRACING "Enable tracing" OFF)
option(SACCADE_WINDOWS_UIACCESS
    "Request signed assistive-technology UIAccess in the Windows application" OFF)
option(SACCADE_MSVC_STATIC_RUNTIME
    "Link the MSVC runtime statically for self-contained Windows packages" OFF)
option(SACCADE_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
set(SACCADE_WINDOWS_ML_ROOT "" CACHE PATH
    "Extracted Microsoft.Windows.AI.MachineLearning package root")
set(SACCADE_MODEL_ARTIFACT "" CACHE FILEPATH
    "Signed saccade.model artifact to bundle with the desktop application")
set(SACCADE_COREML_MODEL_BUNDLE "" CACHE PATH
    "Compiled .mlmodelc directory referenced by the macOS model artifact")
set(SACCADE_MACOS_CODESIGN_IDENTITY "-" CACHE STRING
    "macOS identity. Use - only for builds that do not need persistent privacy grants")
set(SACCADE_MACOS_NOTARY_PROFILE "" CACHE STRING
    "Keychain profile used by xcrun notarytool for distribution packages")
option(SACCADE_MACOS_HARDENED_RUNTIME
    "Enable the hardened runtime on the macOS application" ON)
set(SACCADE_WINDOWS_SIGNTOOL "" CACHE FILEPATH
    "Path to signtool.exe for Windows distribution signing")
set(SACCADE_WINDOWS_SIGNING_CERTIFICATE_SHA1 "" CACHE STRING
    "SHA-1 thumbprint of the Windows Authenticode certificate")
set(SACCADE_WINDOWS_TIMESTAMP_URL "http://timestamp.digicert.com" CACHE STRING
    "RFC 3161 timestamp service for Windows distribution signing")
