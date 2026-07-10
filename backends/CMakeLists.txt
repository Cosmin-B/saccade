if(SACCADE_BACKEND_MOCK)
    add_subdirectory(mock)
endif()

if(SACCADE_BACKEND_REFERENCE_CPU)
    add_subdirectory(reference_cpu)
endif()
