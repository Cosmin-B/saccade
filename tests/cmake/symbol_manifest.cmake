if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(manifest "${SOURCE_DIR}/abi/symbols-v1.txt")
file(STRINGS "${manifest}" manifest_symbols REGEX "^saccade_[a-z0-9_]+$")
if(NOT manifest_symbols)
    message(FATAL_ERROR "The public symbol manifest is empty")
endif()

set(unique_manifest ${manifest_symbols})
list(REMOVE_DUPLICATES unique_manifest)
list(LENGTH manifest_symbols manifest_count)
list(LENGTH unique_manifest unique_count)
if(NOT manifest_count EQUAL unique_count)
    message(FATAL_ERROR "The public symbol manifest contains duplicates")
endif()

file(READ "${SOURCE_DIR}/include/saccade/saccade.h" core_header)
file(READ "${SOURCE_DIR}/include/saccade/saccade_backend.h" backend_header)
set(headers "${core_header}\n${backend_header}")
string(REGEX MATCHALL
    "SACCADE_API[^\n]*saccade_[a-z0-9_]+\\("
    declarations "${headers}")

set(header_symbols)
foreach(declaration IN LISTS declarations)
    string(REGEX MATCH "saccade_[a-z0-9_]+" symbol "${declaration}")
    list(APPEND header_symbols "${symbol}")
endforeach()

list(SORT manifest_symbols)
list(SORT header_symbols)
if(NOT "${manifest_symbols}" STREQUAL "${header_symbols}")
    message(FATAL_ERROR
        "Public headers and symbol manifest differ.\n"
        "Manifest: ${manifest_symbols}\nHeaders: ${header_symbols}")
endif()

string(REGEX MATCHALL
    "typedef (u?int(32|64)_t) (Saccade[A-Za-z0-9_]+);"
    fixed_type_declarations "${headers}")
string(REGEX MATCHALL
    "typedef struct (Saccade[A-Za-z0-9_]+)"
    structure_declarations "${headers}")
set(public_layout_types)
foreach(declaration IN LISTS fixed_type_declarations structure_declarations)
    string(REGEX MATCH "Saccade[A-Za-z0-9_]+" type "${declaration}")
    if(type)
        list(APPEND public_layout_types "${type}")
    endif()
endforeach()
list(REMOVE_DUPLICATES public_layout_types)
list(SORT public_layout_types)

file(READ "${SOURCE_DIR}/abi/layout-v1.def" layout_manifest)
string(REGEX MATCHALL
    "SACCADE_ABI_SIZE\\(Saccade[A-Za-z0-9_]+,"
    layout_size_declarations "${layout_manifest}")
set(manifest_layout_types)
foreach(declaration IN LISTS layout_size_declarations)
    string(REGEX MATCH "Saccade[A-Za-z0-9_]+" type "${declaration}")
    list(APPEND manifest_layout_types "${type}")
endforeach()
list(REMOVE_DUPLICATES manifest_layout_types)
list(SORT manifest_layout_types)

if(NOT "${public_layout_types}" STREQUAL "${manifest_layout_types}")
    message(FATAL_ERROR
        "Public type inventory and layout manifest differ.\n"
        "Headers: ${public_layout_types}\nManifest: ${manifest_layout_types}")
endif()
