set(SACCADE_WINDOWS_ML_REQUIRED_VERSION "2.1.74")
set(SACCADE_WINDOWS_ML_PACKAGE_SHA256
    "691165fa3c07a04b752cbf4a07e93ed13a418e9dea1ee89eb163d2225e2ba3af")

function(saccade_verify_windows_ml_runtime)
    if(NOT "${WINML_VERSION}" STREQUAL "${SACCADE_WINDOWS_ML_REQUIRED_VERSION}")
        message(FATAL_ERROR
            "Saccade requires Microsoft.Windows.AI.MachineLearning "
            "${SACCADE_WINDOWS_ML_REQUIRED_VERSION}; received ${WINML_VERSION}")
    endif()

    if("${WINML_BINARY_DIR}" MATCHES "win-x64")
        set(_saccade_ort_sha256
            "3cffeff2d7c25b247a814212baab70eb1f37d727335d4c813ed73785df80a794")
        set(_saccade_directml_sha256
            "257c75b2f607940c986d0b96d9309a2c897e57ef3192b6c678d707c22d747611")
    elseif("${WINML_BINARY_DIR}" MATCHES "win-arm64ec")
        set(_saccade_ort_sha256
            "58e3e0229e6d0af540f64179e2fadfb23b54465dfe0cdd5bdb1d93df2acabc15")
        set(_saccade_directml_sha256
            "5a852a6b18166cf00413a8b2f0b5da6b67b77f8c73e7d0af0f2f6f8b9eea4cc4")
    elseif("${WINML_BINARY_DIR}" MATCHES "win-arm64")
        set(_saccade_ort_sha256
            "3e5959cfcdd1c9c09586c7ec27c547270a95182df2d060ece5650c317628e79e")
        set(_saccade_directml_sha256
            "08b031bcd5f79f7aa5cfc140806ddcf90cc2d43c6b2efc77982c18d926adf730")
    else()
        message(FATAL_ERROR "Unsupported Windows ML runtime directory: ${WINML_BINARY_DIR}")
    endif()

    if(NOT "${WINML_ONNXRUNTIME_PROVIDERS_SHARED_DLL}" STREQUAL "")
        message(FATAL_ERROR
            "Windows ML ${SACCADE_WINDOWS_ML_REQUIRED_VERSION} does not ship "
            "onnxruntime_providers_shared.dll; refusing an unpinned runtime from "
            "${WINML_ONNXRUNTIME_PROVIDERS_SHARED_DLL}")
    endif()

    foreach(_saccade_runtime IN ITEMS onnxruntime.dll DirectML.dll)
        if(_saccade_runtime STREQUAL "onnxruntime.dll")
            set(_saccade_expected_sha256 "${_saccade_ort_sha256}")
        else()
            set(_saccade_expected_sha256 "${_saccade_directml_sha256}")
        endif()

        set(_saccade_runtime_path "${WINML_BINARY_DIR}/${_saccade_runtime}")
        if(NOT EXISTS "${_saccade_runtime_path}")
            message(FATAL_ERROR "Windows ML runtime is missing ${_saccade_runtime_path}")
        endif()
        file(SHA256 "${_saccade_runtime_path}" _saccade_actual_sha256)
        if(NOT "${_saccade_actual_sha256}" STREQUAL "${_saccade_expected_sha256}")
            message(FATAL_ERROR
                "Windows ML ${_saccade_runtime} failed SHA-256 verification: "
                "expected ${_saccade_expected_sha256}, received ${_saccade_actual_sha256}")
        endif()
    endforeach()

    message(STATUS
        "Windows ML ${WINML_VERSION} runtime verified (${WINML_BINARY_DIR})")
endfunction()
