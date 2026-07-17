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
    if(path MATCHES "/[.]git/" OR path MATCHES "/build(-[^/]+)?/")
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

file(GLOB_RECURSE runtime_sources LIST_DIRECTORIES false
    "${SOURCE_DIR}/backends/*.c"
    "${SOURCE_DIR}/backends/*.cc"
    "${SOURCE_DIR}/backends/*.cpp"
    "${SOURCE_DIR}/backends/*.cxx"
    "${SOURCE_DIR}/backends/*.mm"
    "${SOURCE_DIR}/backends/*.h"
    "${SOURCE_DIR}/backends/*.hpp"
    "${SOURCE_DIR}/platform/*.c"
    "${SOURCE_DIR}/platform/*.cc"
    "${SOURCE_DIR}/platform/*.cpp"
    "${SOURCE_DIR}/platform/*.cxx"
    "${SOURCE_DIR}/platform/*.mm"
    "${SOURCE_DIR}/platform/*.h"
    "${SOURCE_DIR}/platform/*.hpp"
    "${SOURCE_DIR}/src/*.c"
    "${SOURCE_DIR}/src/*.cc"
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/src/*.cxx"
    "${SOURCE_DIR}/src/*.h"
    "${SOURCE_DIR}/src/*.hpp")
foreach(path IN LISTS runtime_sources)
    file(READ "${path}" content)
    string(REGEX MATCH
        "(^|[^A-Za-z0-9_])(malloc|calloc|realloc|aligned_alloc|posix_memalign|_aligned_malloc|make_unique|make_shared)[ \t\r\n]*\\("
        direct_allocation "${content}")
    if(direct_allocation)
        message(FATAL_ERROR "Runtime source uses a direct allocator: ${path}")
    endif()
endforeach()

file(GLOB_RECURSE cpp_sources LIST_DIRECTORIES false
    "${SOURCE_DIR}/backends/*.cc"
    "${SOURCE_DIR}/backends/*.cpp"
    "${SOURCE_DIR}/backends/*.cxx"
    "${SOURCE_DIR}/backends/*.mm"
    "${SOURCE_DIR}/backends/*.hpp"
    "${SOURCE_DIR}/platform/*.cc"
    "${SOURCE_DIR}/platform/*.cpp"
    "${SOURCE_DIR}/platform/*.cxx"
    "${SOURCE_DIR}/platform/*.mm"
    "${SOURCE_DIR}/platform/*.hpp"
    "${SOURCE_DIR}/include/*.hpp"
    "${SOURCE_DIR}/src/*.cc"
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/src/*.cxx"
    "${SOURCE_DIR}/src/*.hpp"
    "${SOURCE_DIR}/tests/*.cc"
    "${SOURCE_DIR}/tests/*.cpp"
    "${SOURCE_DIR}/tests/*.cxx"
    "${SOURCE_DIR}/tests/*.hpp")
foreach(path IN LISTS cpp_sources)
    file(READ "${path}" content)
    if(content MATCHES "std[ \\t\\r\\n]*::[ \\t\\r\\n]*function" OR
       content MATCHES "#[ \\t]*include[ \\t]*<functional>")
        message(FATAL_ERROR "C++ source uses an allocating callback wrapper: ${path}")
    endif()
    if((content MATCHES "std[ \t\r\n]*::[ \t\r\n]*mutex" OR
        content MATCHES "std[ \t\r\n]*::[ \t\r\n]*(lock_guard|unique_lock|scoped_lock)" OR
        content MATCHES "#[ \t]*include[ \t]*<mutex>") )
        message(FATAL_ERROR "C++ source uses a mutex instead of owned state: ${path}")
    endif()
    if(content MATCHES "compare_exchange_(weak|strong)" AND
       NOT path STREQUAL "${SOURCE_DIR}/src/core/rare_global_gate.hpp")
        message(FATAL_ERROR "C++ source uses a CAS primitive instead of thread-owned state: ${path}")
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
