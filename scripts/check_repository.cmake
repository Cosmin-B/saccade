if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE source_files LIST_DIRECTORIES false "${SOURCE_DIR}/*")
set(blocked_names
    .DS_Store
    CMakeCache.txt
    compile_commands.json)
set(blocked_extensions
    .ipynb
    .mlmodel
    .onnx
    .ort
    .pt
    .pth
    .pyc)

foreach(path IN LISTS source_files)
    if(path MATCHES "/[.]git/" OR path MATCHES "/build/")
        continue()
    endif()
    get_filename_component(name "${path}" NAME)
    get_filename_component(extension "${path}" EXT)
    if(name IN_LIST blocked_names OR extension IN_LIST blocked_extensions OR
       path MATCHES "/__pycache__/")
        message(FATAL_ERROR "Generated or local artifact is in the source tree: ${path}")
    endif()
    file(SIZE "${path}" size)
    if(size GREATER 1048576)
        message(FATAL_ERROR "Source artifact exceeds the 1 MiB review limit: ${path}")
    endif()
endforeach()

file(GLOB_RECURSE backend_sources LIST_DIRECTORIES false
    "${SOURCE_DIR}/backends/*.c"
    "${SOURCE_DIR}/backends/*.cc"
    "${SOURCE_DIR}/backends/*.cpp"
    "${SOURCE_DIR}/backends/*.cxx"
    "${SOURCE_DIR}/backends/*.h"
    "${SOURCE_DIR}/backends/*.hpp")
foreach(path IN LISTS backend_sources)
    file(READ "${path}" content)
    string(REGEX MATCH
        "(^|[^A-Za-z0-9_])(malloc|calloc|realloc|aligned_alloc|posix_memalign|_aligned_malloc|make_unique|make_shared)[ \t\r\n]*\\("
        direct_allocation "${content}")
    if(direct_allocation)
        message(FATAL_ERROR "Provider source uses a direct allocator: ${path}")
    endif()
endforeach()

file(GLOB_RECURSE build_files LIST_DIRECTORIES false
    "${SOURCE_DIR}/*.cmake"
    "${SOURCE_DIR}/CMakeLists.txt")
foreach(path IN LISTS build_files)
    if(path MATCHES "/build/" OR path STREQUAL CMAKE_CURRENT_LIST_FILE)
        continue()
    endif()
    file(READ "${path}" content)
    foreach(command IN ITEMS FetchContent_Declare ExternalProject_Add "file(DOWNLOAD")
        string(FIND "${content}" "${command}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "Build configuration downloads a dependency: ${path}")
        endif()
    endforeach()
endforeach()
