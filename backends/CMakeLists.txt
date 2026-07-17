if(SACCADE_BACKEND_MOCK)
    add_subdirectory(mock)
endif()

if(SACCADE_BACKEND_REFERENCE_CPU)
    add_subdirectory(reference_cpu)
endif()

if(APPLE AND SACCADE_BACKEND_METAL)
    enable_language(OBJCXX)
    add_subdirectory(metal)
endif()

if(WIN32 AND SACCADE_BACKEND_D3D12)
    add_subdirectory(d3d12)
endif()
