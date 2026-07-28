cmake_minimum_required(VERSION 3.22)

# Public runtime baselines are build contracts, not documentation-only labels. A package that
# silently inherits a newer host SDK target would no longer run on the advertised minimum.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING
        "Minimum supported macOS version" FORCE)
endif()

project(
    gdpp
    VERSION 1.8.2
    DESCRIPTION "Ahead-of-time compiler for GDScript"
    LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are forbidden. Use cmake --preset dev.")
endif()

file(REAL_PATH "${CMAKE_SOURCE_DIR}/build" GDPP_BUILD_ROOT)
file(REAL_PATH "${CMAKE_BINARY_DIR}" GDPP_CURRENT_BUILD_DIR)
string(FIND "${GDPP_CURRENT_BUILD_DIR}" "${GDPP_BUILD_ROOT}/" GDPP_BUILD_PREFIX)
if(NOT GDPP_CURRENT_BUILD_DIR STREQUAL GDPP_BUILD_ROOT AND NOT GDPP_BUILD_PREFIX EQUAL 0)
    message(FATAL_ERROR "All CMake intermediate products must live under ${GDPP_BUILD_ROOT}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(GDPP_MIN_WINDOWS_VERSION "10")
set(GDPP_MIN_MACOS_VERSION "11.0")
set(GDPP_MIN_LINUX_DISTRIBUTION "Ubuntu 22.04")
set(GDPP_MIN_ANDROID_VERSION "9")
set(GDPP_MIN_ANDROID_API_LEVEL "28")
set(GDPP_MIN_IOS_VERSION "16.0")
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# godot-cpp links the MSVC runtime statically by default.  This must be selected before any GDPP
# target is created: changing it only when godot-cpp is added leaves gdpp_core on /MD and the
# extension/generated project on /MT, which produces LNK2038 (or worse, crosses CRT heaps).
if(MSVC)
    # A single godot-cpp target contains thousands of translation units. Visual Studio may launch
    # several compiler processes for that target even though SDK variants themselves are ordered;
    # /FS routes compile-PDB writes through MSPDBSRV instead of allowing C2471 contention.
    add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/FS>)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING
        "MSVC runtime shared by GDPP, godot-cpp and generated project libraries" FORCE)
endif()
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# godot-cpp imports Python modules while generating bindings. Keep bytecode caches out of the
# pinned source submodule so every intermediate artifact remains below the root build directory.
set(ENV{PYTHONDONTWRITEBYTECODE} "1")
# RULE_LAUNCH_CUSTOM is a raw shell fragment rather than an argv list, so VERBATIM cannot quote
# the launcher executable for us.  Keep the quotes explicit: hosted Windows CMake installations
# live below "Program Files", and an unquoted launcher otherwise fails before any custom command
# can start.
set_property(GLOBAL PROPERTY RULE_LAUNCH_CUSTOM
    "\"${CMAKE_COMMAND}\" -E env PYTHONDONTWRITEBYTECODE=1"
)

option(GDPP_BUILD_CLI "Build the gdpp command-line compiler" ON)
option(GDPP_BUILD_TESTS "Build compiler tests" ON)
option(GDPP_BUILD_FUZZERS "Build coverage-guided compiler fuzz targets" OFF)
option(GDPP_BUILD_EXTENSION "Build the Godot GDExtension" OFF)
option(GDPP_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(GDPP_ENABLE_COMPATIBILITY_CORPUS_TESTS
    "Download and test the project and parser compatibility corpora" OFF)
option(GDPP_SERIALIZE_SDK_BINDING_BUILDS
    "Build complete godot-cpp SDK variants sequentially while retaining each variant's internal parallelism"
    ON)

# Public Windows packages use one compiler and CRT contract.  Accepting MinGW here would create
# SDK archives that cannot be linked safely into the MSVC-based commercial project pipeline.
if(WIN32 AND GDPP_BUILD_EXTENSION AND CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    message(FATAL_ERROR "GDPP commercial Windows release packages require MSVC")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND GDPP_BUILD_EXTENSION AND
        CMAKE_BUILD_TYPE STREQUAL "Release")
    execute_process(
        COMMAND getconf GNU_LIBC_VERSION
        OUTPUT_VARIABLE GDPP_HOST_GLIBC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GDPP_GLIBC_QUERY_STATUS
    )
    if(NOT GDPP_GLIBC_QUERY_STATUS EQUAL 0 OR
            NOT GDPP_HOST_GLIBC MATCHES "glibc ([0-9]+\\.[0-9]+)")
        message(FATAL_ERROR "Cannot determine glibc for the Ubuntu 22.04 release baseline")
    endif()
    if(CMAKE_MATCH_1 VERSION_GREATER "2.35")
        message(FATAL_ERROR
            "Release Linux plugins must be built against glibc 2.35 or older; "
            "use the Ubuntu 22.04 release runner (host reports ${GDPP_HOST_GLIBC})")
    endif()
endif()

set(GDPP_MIN_GODOT_VERSION "4.4")
set(GDPP_SUPPORTED_GODOT_VERSIONS "4.4;4.5;4.6;4.7")
set(GDPP_PACKAGE_GODOT_VERSIONS "${GDPP_SUPPORTED_GODOT_VERSIONS}" CACHE STRING
    "Godot SDK versions included in the end-user plugin package")
set(GDPP_GODOT_PRECISION "single" CACHE STRING
    "Godot real_t ABI used by the compiler plugin and every packaged SDK (single|double)")
set_property(CACHE GDPP_GODOT_PRECISION PROPERTY STRINGS "single;double")
set(GDPP_CUSTOM_GDEXTENSION_API_FILE "" CACHE FILEPATH
    "Custom Godot extension_api.json used to build an exact single-version GDPP package")
if(NOT GDPP_GODOT_PRECISION MATCHES "^(single|double)$")
    message(FATAL_ERROR
        "GDPP_GODOT_PRECISION must be 'single' or 'double', got '${GDPP_GODOT_PRECISION}'")
endif()

set(GDPP_GODOT_API_KIND "official")
if(GDPP_CUSTOM_GDEXTENSION_API_FILE)
    file(REAL_PATH "${GDPP_CUSTOM_GDEXTENSION_API_FILE}" GDPP_CUSTOM_GDEXTENSION_API_FILE
        BASE_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${GDPP_CUSTOM_GDEXTENSION_API_FILE}")
        message(FATAL_ERROR
            "Custom Godot extension API does not exist: ${GDPP_CUSTOM_GDEXTENSION_API_FILE}")
    endif()
    file(READ "${GDPP_CUSTOM_GDEXTENSION_API_FILE}" GDPP_CUSTOM_GDEXTENSION_API_JSON)
    string(JSON GDPP_CUSTOM_API_MAJOR ERROR_VARIABLE GDPP_CUSTOM_API_MAJOR_ERROR
        GET "${GDPP_CUSTOM_GDEXTENSION_API_JSON}" header version_major)
    string(JSON GDPP_CUSTOM_API_MINOR ERROR_VARIABLE GDPP_CUSTOM_API_MINOR_ERROR
        GET "${GDPP_CUSTOM_GDEXTENSION_API_JSON}" header version_minor)
    if(NOT GDPP_CUSTOM_API_MAJOR_ERROR STREQUAL "NOTFOUND" OR
            NOT GDPP_CUSTOM_API_MINOR_ERROR STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Custom Godot extension API is missing header.version_major/version_minor")
    endif()
    set(GDPP_CUSTOM_API_VERSION "${GDPP_CUSTOM_API_MAJOR}.${GDPP_CUSTOM_API_MINOR}")
    if(NOT GDPP_CUSTOM_API_VERSION IN_LIST GDPP_SUPPORTED_GODOT_VERSIONS)
        message(FATAL_ERROR
            "Custom Godot extension API ${GDPP_CUSTOM_API_VERSION} is outside the supported "
            "4.4-4.7 ABI matrix")
    endif()
    string(JSON GDPP_CUSTOM_API_PRECISION ERROR_VARIABLE GDPP_CUSTOM_API_PRECISION_ERROR
        GET "${GDPP_CUSTOM_GDEXTENSION_API_JSON}" header precision)
    if(GDPP_CUSTOM_API_PRECISION_ERROR STREQUAL "NOTFOUND" AND
            NOT GDPP_CUSTOM_API_PRECISION STREQUAL GDPP_GODOT_PRECISION)
        message(FATAL_ERROR
            "Custom Godot extension API precision is '${GDPP_CUSTOM_API_PRECISION}', but "
            "GDPP_GODOT_PRECISION is '${GDPP_GODOT_PRECISION}'")
    endif()
    unset(GDPP_CUSTOM_GDEXTENSION_API_JSON)
    set(GDPP_GODOT_API_KIND "custom")
    set(GDPP_GODOT_API_VERSION "${GDPP_CUSTOM_API_VERSION}" CACHE STRING
        "Godot API used to build the compiler extension" FORCE)
    set(GODOTCPP_CUSTOM_API_FILE "${GDPP_CUSTOM_GDEXTENSION_API_FILE}" CACHE FILEPATH
        "Exact custom Godot API shared by GDPP and godot-cpp" FORCE)
else()
    if(GDPP_GODOT_PRECISION STREQUAL "double")
        message(FATAL_ERROR
            "Double-precision GDPP builds require GDPP_CUSTOM_GDEXTENSION_API_FILE exported "
            "by the matching double-precision Godot build")
    endif()
    set(GDPP_GODOT_API_VERSION "${GDPP_MIN_GODOT_VERSION}" CACHE STRING
        "Godot API used to build the compiler extension" FORCE)
    set(GODOTCPP_CUSTOM_API_FILE "" CACHE FILEPATH
        "Exact custom Godot API shared by GDPP and godot-cpp" FORCE)
endif()
set(GODOTCPP_PRECISION "${GDPP_GODOT_PRECISION}" CACHE STRING
    "Godot real_t precision shared by every GDPP native target" FORCE)

if(NOT GDPP_PACKAGE_GODOT_VERSIONS)
    message(FATAL_ERROR "GDPP_PACKAGE_GODOT_VERSIONS must contain at least one supported version")
endif()
list(REMOVE_DUPLICATES GDPP_PACKAGE_GODOT_VERSIONS)
foreach(GDPP_PACKAGE_VERSION IN LISTS GDPP_PACKAGE_GODOT_VERSIONS)
    if(NOT GDPP_PACKAGE_VERSION IN_LIST GDPP_SUPPORTED_GODOT_VERSIONS)
        message(FATAL_ERROR
            "Unsupported packaged Godot SDK ${GDPP_PACKAGE_VERSION}; expected one of: "
            "${GDPP_SUPPORTED_GODOT_VERSIONS}")
    endif()
endforeach()
if(GDPP_CUSTOM_GDEXTENSION_API_FILE)
    list(LENGTH GDPP_PACKAGE_GODOT_VERSIONS GDPP_CUSTOM_PACKAGE_VERSION_COUNT)
    if(NOT GDPP_CUSTOM_PACKAGE_VERSION_COUNT EQUAL 1 OR
            NOT GDPP_CUSTOM_API_VERSION IN_LIST GDPP_PACKAGE_GODOT_VERSIONS)
        message(FATAL_ERROR
            "A custom Godot build must package exactly its own ${GDPP_CUSTOM_API_VERSION} SDK; "
            "set GDPP_PACKAGE_GODOT_VERSIONS=${GDPP_CUSTOM_API_VERSION}")
    endif()
endif()
set(GDPP_GODOT_EXECUTABLE "" CACHE FILEPATH "Optional Godot executable used for integration tests")

# Resolve every semantic API table before deriving the compiler fingerprint and version header.
# A custom snapshot replaces exactly its matching minor; all other tables remain the pinned
# official corpus so repository tests can still audit the complete supported version matrix.
foreach(GDPP_API_VERSION IN LISTS GDPP_SUPPORTED_GODOT_VERSIONS)
    string(REPLACE "." "_" GDPP_API_SUFFIX "${GDPP_API_VERSION}")
    if(GDPP_CUSTOM_GDEXTENSION_API_FILE AND
            GDPP_API_VERSION STREQUAL GDPP_CUSTOM_API_VERSION)
        set(GDPP_API_JSON "${GDPP_CUSTOM_GDEXTENSION_API_FILE}")
        set(GDPP_API_KIND "custom")
        set(GDPP_API_PRECISION "${GDPP_GODOT_PRECISION}")
    elseif(GDPP_API_VERSION STREQUAL "4.7")
        set(GDPP_API_JSON
            "${CMAKE_SOURCE_DIR}/third/godot-cpp/gdextension/extension_api.json")
        set(GDPP_API_KIND "official")
        set(GDPP_API_PRECISION "single")
    else()
        string(REPLACE "." "-" GDPP_API_DASHED "${GDPP_API_VERSION}")
        set(GDPP_API_JSON
            "${CMAKE_SOURCE_DIR}/third/godot-cpp/gdextension/extension_api-${GDPP_API_DASHED}.json")
        set(GDPP_API_KIND "official")
        set(GDPP_API_PRECISION "single")
    endif()
    file(SHA256 "${GDPP_API_JSON}" GDPP_API_SHA256)
    set(GDPP_GODOT_API_FILE_${GDPP_API_SUFFIX} "${GDPP_API_JSON}")
    set(GDPP_GODOT_API_KIND_${GDPP_API_SUFFIX} "${GDPP_API_KIND}")
    set(GDPP_GODOT_API_PRECISION_${GDPP_API_SUFFIX} "${GDPP_API_PRECISION}")
    set(GDPP_GODOT_API_SHA256_${GDPP_API_SUFFIX} "${GDPP_API_SHA256}")
endforeach()

# The generated C++ and the compiler-only SDK form one native runtime contract.  Keep exact
# content hashes in the compiler as well as every packaged SDK so an old or partially copied SDK
# is rejected during preflight instead of failing later with thousands of C++ diagnostics.
set(GDPP_NATIVE_SDK_SCHEMA 12)
set(GDPP_NATIVE_RUNTIME_ABI 20)
set(GDPP_NATIVE_RUNTIME_HEADER
    "${CMAKE_SOURCE_DIR}/include/gdpp/runtime/variant_ops.hpp")
set(GDPP_REFERENCE_SEMANTICS_HEADER
    "${CMAKE_SOURCE_DIR}/include/gdpp/runtime/reference_semantics.hpp")
set(GDPP_NATIVE_RUNTIME_SOURCE
    "${CMAKE_SOURCE_DIR}/src/runtime/variant_ops.cpp")
set(GDPP_ATTACHED_RUNTIME_HEADER
    "${CMAKE_SOURCE_DIR}/include/gdpp/runtime/attached_script.hpp")
set(GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE
    "${CMAKE_SOURCE_DIR}/src/runtime/attached_script_registry.cpp")
set(GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE
    "${CMAKE_SOURCE_DIR}/src/runtime/attached_script_instance.cpp")
set(GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE
    "${CMAKE_SOURCE_DIR}/src/runtime/attached_script_language.cpp")
set(GDPP_INTEGER_SEMANTICS_HEADER
    "${CMAKE_SOURCE_DIR}/include/gdpp/numeric/integer_semantics.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GDPP_NATIVE_RUNTIME_HEADER}"
    "${GDPP_REFERENCE_SEMANTICS_HEADER}"
    "${GDPP_NATIVE_RUNTIME_SOURCE}"
    "${GDPP_ATTACHED_RUNTIME_HEADER}"
    "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
    "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
    "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
    "${GDPP_INTEGER_SEMANTICS_HEADER}"
)
foreach(GDPP_NATIVE_RUNTIME_FILE IN ITEMS
        "${GDPP_NATIVE_RUNTIME_HEADER}"
        "${GDPP_REFERENCE_SEMANTICS_HEADER}"
        "${GDPP_NATIVE_RUNTIME_SOURCE}"
        "${GDPP_ATTACHED_RUNTIME_HEADER}"
        "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
        "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
        "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
        "${GDPP_INTEGER_SEMANTICS_HEADER}")
    file(READ "${GDPP_NATIVE_RUNTIME_FILE}" GDPP_NATIVE_RUNTIME_CONTENT)
    string(FIND "${GDPP_NATIVE_RUNTIME_CONTENT}" "\r" GDPP_NATIVE_RUNTIME_CR_OFFSET)
    if(NOT GDPP_NATIVE_RUNTIME_CR_OFFSET EQUAL -1)
        message(FATAL_ERROR
            "Native SDK contract files must use LF line endings: ${GDPP_NATIVE_RUNTIME_FILE}")
    endif()
endforeach()
unset(GDPP_NATIVE_RUNTIME_CONTENT)
file(SHA256 "${GDPP_NATIVE_RUNTIME_HEADER}" GDPP_NATIVE_RUNTIME_HEADER_SHA256)
file(SHA256 "${GDPP_REFERENCE_SEMANTICS_HEADER}" GDPP_REFERENCE_SEMANTICS_HEADER_SHA256)
file(SHA256 "${GDPP_NATIVE_RUNTIME_SOURCE}" GDPP_NATIVE_RUNTIME_SOURCE_SHA256)
file(SHA256 "${GDPP_ATTACHED_RUNTIME_HEADER}" GDPP_ATTACHED_RUNTIME_HEADER_SHA256)
file(SHA256 "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
    GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE_SHA256)
file(SHA256 "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
    GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE_SHA256)
file(SHA256 "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
    GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE_SHA256)
file(SHA256 "${GDPP_INTEGER_SEMANTICS_HEADER}" GDPP_INTEGER_SEMANTICS_HEADER_SHA256)

# Generated project C++ is a product of the whole compiler contract, not only the input .gd file.
# Derive a deterministic fingerprint from every source and API table that can affect emission so
# an upgraded compiler can never reuse stale translation units from an older plugin binary.
file(GLOB_RECURSE GDPP_CODEGEN_CONTRACT_FILES CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/include/gdpp/*.hpp"
    "${CMAKE_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/compiler/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/frontend/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/frontend/*.inc"
    "${CMAKE_SOURCE_DIR}/src/semantic/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/ir/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/codegen/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/project/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/support/*.cpp"
    "${CMAKE_SOURCE_DIR}/third/godot-cpp/gdextension/extension_api*.json"
)
if(GDPP_CUSTOM_GDEXTENSION_API_FILE)
    list(APPEND GDPP_CODEGEN_CONTRACT_FILES "${GDPP_CUSTOM_GDEXTENSION_API_FILE}")
endif()
list(APPEND GDPP_CODEGEN_CONTRACT_FILES
    "${CMAKE_SOURCE_DIR}/include/gdpp/version.hpp.in"
    "${CMAKE_SOURCE_DIR}/tools/generate_godot_api.py"
    "${CMAKE_SOURCE_DIR}/tools/generate_unicode_identifiers.py"
)
list(REMOVE_DUPLICATES GDPP_CODEGEN_CONTRACT_FILES)
list(SORT GDPP_CODEGEN_CONTRACT_FILES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${GDPP_CODEGEN_CONTRACT_FILES}
)
set(GDPP_CODEGEN_CONTRACT "GDPP_CODEGEN_CONTRACT_V1\n")
foreach(GDPP_CODEGEN_CONTRACT_FILE IN LISTS GDPP_CODEGEN_CONTRACT_FILES)
    file(RELATIVE_PATH GDPP_CODEGEN_CONTRACT_RELATIVE
        "${CMAKE_SOURCE_DIR}" "${GDPP_CODEGEN_CONTRACT_FILE}")
    file(SHA256 "${GDPP_CODEGEN_CONTRACT_FILE}" GDPP_CODEGEN_CONTRACT_FILE_SHA256)
    string(APPEND GDPP_CODEGEN_CONTRACT
        "${GDPP_CODEGEN_CONTRACT_RELATIVE}:${GDPP_CODEGEN_CONTRACT_FILE_SHA256}\n")
endforeach()
string(SHA256 GDPP_CODEGEN_FINGERPRINT "${GDPP_CODEGEN_CONTRACT}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

configure_file(
    include/gdpp/version.hpp.in
    "${CMAKE_BINARY_DIR}/generated/gdpp/version.hpp"
    @ONLY
)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(GDPP_ANDROID_NDK_ROOT "" CACHE PATH
    "Android NDK used to build optional target SDK modules")
set(GDPP_ANDROID_SDK_VERSIONS "" CACHE STRING
    "Godot versions for optional Android target SDK modules")
set(GDPP_ANDROID_ARCHITECTURES "arm64" CACHE STRING
    "Android target SDK architectures (arm64 and/or x86_64)")
set(GDPP_ANDROID_API_LEVEL "${GDPP_MIN_ANDROID_API_LEVEL}" CACHE STRING
    "Fixed Android native ABI baseline" FORCE)
set(GDPP_WEB_SDK_VERSIONS "" CACHE STRING
    "Godot versions for optional Web wasm32 target SDK modules")
set(GDPP_WEB_VARIANTS "nothreads;threads" CACHE STRING
    "Web target SDK variants (nothreads and/or threads)")
set(GDPP_WEB_EMCMAKE "emcmake" CACHE FILEPATH
    "Emscripten CMake launcher used to package Web target SDK modules")
set(GDPP_IOS_SDK_VERSIONS "" CACHE STRING
    "Godot versions for optional iOS device and Universal Simulator target SDK modules")

if(GDPP_ANDROID_SDK_VERSIONS)
    if(NOT GDPP_ANDROID_NDK_ROOT)
        message(FATAL_ERROR "GDPP_ANDROID_NDK_ROOT is required for Android SDK packaging")
    endif()
    if(NOT "${GDPP_ANDROID_API_LEVEL}" STREQUAL "${GDPP_MIN_ANDROID_API_LEVEL}")
        message(FATAL_ERROR "GDPP Android SDK ABI baseline is fixed at API level 28 (Android 9)")
    endif()
    set(GDPP_ANDROID_SDK_TARGETS)
    foreach(GDPP_ANDROID_VERSION IN LISTS GDPP_ANDROID_SDK_VERSIONS)
        if(NOT GDPP_ANDROID_VERSION IN_LIST GDPP_SUPPORTED_GODOT_VERSIONS)
            message(FATAL_ERROR "Unsupported Android SDK Godot version: ${GDPP_ANDROID_VERSION}")
        endif()
        string(REPLACE "." "_" GDPP_ANDROID_VERSION_SUFFIX "${GDPP_ANDROID_VERSION}")
        set(GDPP_ANDROID_API_FILE_VARIABLE
            "GDPP_GODOT_API_FILE_${GDPP_ANDROID_VERSION_SUFFIX}")
        set(GDPP_ANDROID_API_KIND_VARIABLE
            "GDPP_GODOT_API_KIND_${GDPP_ANDROID_VERSION_SUFFIX}")
        set(GDPP_ANDROID_API_SHA256_VARIABLE
            "GDPP_GODOT_API_SHA256_${GDPP_ANDROID_VERSION_SUFFIX}")
        foreach(GDPP_ANDROID_ARCH IN LISTS GDPP_ANDROID_ARCHITECTURES)
            if(NOT GDPP_ANDROID_ARCH MATCHES "^(arm64|x86_64)$")
                message(FATAL_ERROR "Unsupported Android architecture: ${GDPP_ANDROID_ARCH}")
            endif()
            set(GDPP_ANDROID_TARGET
                "gdpp_android_sdk_${GDPP_ANDROID_VERSION_SUFFIX}_${GDPP_ANDROID_ARCH}")
            add_custom_target(
                ${GDPP_ANDROID_TARGET}
                COMMAND
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build_android_sdk.py"
                    --source-root "${CMAKE_SOURCE_DIR}"
                    --build-root "${CMAKE_BINARY_DIR}/android-sdk"
                    --addon-root "${CMAKE_SOURCE_DIR}/example/addons/gdpp"
                    --ndk-root "${GDPP_ANDROID_NDK_ROOT}"
                    --godot-version "${GDPP_ANDROID_VERSION}"
                    --api-file "${${GDPP_ANDROID_API_FILE_VARIABLE}}"
                    --api-kind "${${GDPP_ANDROID_API_KIND_VARIABLE}}"
                    --api-sha256 "${${GDPP_ANDROID_API_SHA256_VARIABLE}}"
                    --precision "${GDPP_GODOT_PRECISION}"
                    --architecture "${GDPP_ANDROID_ARCH}"
                    --api-level "${GDPP_ANDROID_API_LEVEL}"
                    --schema "${GDPP_NATIVE_SDK_SCHEMA}"
                    --runtime-abi "${GDPP_NATIVE_RUNTIME_ABI}"
                    --gdpp-version "${PROJECT_VERSION}"
                DEPENDS
                    "${CMAKE_SOURCE_DIR}/tools/build_android_sdk.py"
                    "${GDPP_NATIVE_RUNTIME_HEADER}"
                    "${GDPP_REFERENCE_SEMANTICS_HEADER}"
                    "${GDPP_NATIVE_RUNTIME_SOURCE}"
                    "${GDPP_ATTACHED_RUNTIME_HEADER}"
                    "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
                    "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
                    "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
                    "${GDPP_INTEGER_SEMANTICS_HEADER}"
                COMMENT
                    "Packaging Godot ${GDPP_ANDROID_VERSION} Android/${GDPP_ANDROID_ARCH} SDK"
                VERBATIM
            )
            list(APPEND GDPP_ANDROID_SDK_TARGETS "${GDPP_ANDROID_TARGET}")
        endforeach()
    endforeach()
    add_custom_target(gdpp_android_sdk DEPENDS ${GDPP_ANDROID_SDK_TARGETS})
endif()

if(GDPP_WEB_SDK_VERSIONS)
    set(GDPP_WEB_SDK_TARGETS)
    foreach(GDPP_WEB_VERSION IN LISTS GDPP_WEB_SDK_VERSIONS)
        if(NOT GDPP_WEB_VERSION IN_LIST GDPP_SUPPORTED_GODOT_VERSIONS)
            message(FATAL_ERROR "Unsupported Web SDK Godot version: ${GDPP_WEB_VERSION}")
        endif()
        string(REPLACE "." "_" GDPP_WEB_VERSION_SUFFIX "${GDPP_WEB_VERSION}")
        set(GDPP_WEB_API_FILE_VARIABLE "GDPP_GODOT_API_FILE_${GDPP_WEB_VERSION_SUFFIX}")
        set(GDPP_WEB_API_KIND_VARIABLE "GDPP_GODOT_API_KIND_${GDPP_WEB_VERSION_SUFFIX}")
        set(GDPP_WEB_API_SHA256_VARIABLE "GDPP_GODOT_API_SHA256_${GDPP_WEB_VERSION_SUFFIX}")
        foreach(GDPP_WEB_VARIANT IN LISTS GDPP_WEB_VARIANTS)
            if(NOT GDPP_WEB_VARIANT MATCHES "^(nothreads|threads)$")
                message(FATAL_ERROR "Unsupported Web SDK variant: ${GDPP_WEB_VARIANT}")
            endif()
            set(GDPP_WEB_TARGET
                "gdpp_web_sdk_${GDPP_WEB_VERSION_SUFFIX}_${GDPP_WEB_VARIANT}")
            add_custom_target(
                ${GDPP_WEB_TARGET}
                COMMAND
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build_web_sdk.py"
                    --source-root "${CMAKE_SOURCE_DIR}"
                    --build-root "${CMAKE_BINARY_DIR}/web-sdk"
                    --addon-root "${CMAKE_SOURCE_DIR}/example/addons/gdpp"
                    --godot-version "${GDPP_WEB_VERSION}"
                    --api-file "${${GDPP_WEB_API_FILE_VARIABLE}}"
                    --api-kind "${${GDPP_WEB_API_KIND_VARIABLE}}"
                    --api-sha256 "${${GDPP_WEB_API_SHA256_VARIABLE}}"
                    --precision "${GDPP_GODOT_PRECISION}"
                    --variant "${GDPP_WEB_VARIANT}"
                    --emcmake "${GDPP_WEB_EMCMAKE}"
                    --schema "${GDPP_NATIVE_SDK_SCHEMA}"
                    --runtime-abi "${GDPP_NATIVE_RUNTIME_ABI}"
                    --gdpp-version "${PROJECT_VERSION}"
                DEPENDS
                    "${CMAKE_SOURCE_DIR}/tools/build_web_sdk.py"
                    "${GDPP_NATIVE_RUNTIME_HEADER}"
                    "${GDPP_REFERENCE_SEMANTICS_HEADER}"
                    "${GDPP_NATIVE_RUNTIME_SOURCE}"
                    "${GDPP_ATTACHED_RUNTIME_HEADER}"
                    "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
                    "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
                    "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
                    "${GDPP_INTEGER_SEMANTICS_HEADER}"
                COMMENT
                    "Packaging Godot ${GDPP_WEB_VERSION} Web/wasm32/${GDPP_WEB_VARIANT} SDK"
                VERBATIM
            )
            list(APPEND GDPP_WEB_SDK_TARGETS "${GDPP_WEB_TARGET}")
        endforeach()
    endforeach()
    add_custom_target(gdpp_web_sdk DEPENDS ${GDPP_WEB_SDK_TARGETS})
endif()

if(GDPP_IOS_SDK_VERSIONS)
    if(NOT APPLE)
        message(FATAL_ERROR "iOS SDK packaging requires macOS and Xcode")
    endif()
    set(GDPP_IOS_SDK_TARGETS)
    foreach(GDPP_IOS_VERSION IN LISTS GDPP_IOS_SDK_VERSIONS)
        if(NOT GDPP_IOS_VERSION IN_LIST GDPP_SUPPORTED_GODOT_VERSIONS)
            message(FATAL_ERROR "Unsupported iOS SDK Godot version: ${GDPP_IOS_VERSION}")
        endif()
        string(REPLACE "." "_" GDPP_IOS_VERSION_SUFFIX "${GDPP_IOS_VERSION}")
        set(GDPP_IOS_API_FILE_VARIABLE "GDPP_GODOT_API_FILE_${GDPP_IOS_VERSION_SUFFIX}")
        set(GDPP_IOS_API_KIND_VARIABLE "GDPP_GODOT_API_KIND_${GDPP_IOS_VERSION_SUFFIX}")
        set(GDPP_IOS_API_SHA256_VARIABLE "GDPP_GODOT_API_SHA256_${GDPP_IOS_VERSION_SUFFIX}")
        set(GDPP_IOS_TARGET "gdpp_ios_sdk_${GDPP_IOS_VERSION_SUFFIX}_arm64")
        add_custom_target(
            ${GDPP_IOS_TARGET}
            COMMAND
                "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build_ios_sdk.py"
                --source-root "${CMAKE_SOURCE_DIR}"
                --build-root "${CMAKE_BINARY_DIR}/ios-sdk"
                --addon-root "${CMAKE_SOURCE_DIR}/example/addons/gdpp"
                --godot-version "${GDPP_IOS_VERSION}"
                --api-file "${${GDPP_IOS_API_FILE_VARIABLE}}"
                --api-kind "${${GDPP_IOS_API_KIND_VARIABLE}}"
                --api-sha256 "${${GDPP_IOS_API_SHA256_VARIABLE}}"
                --precision "${GDPP_GODOT_PRECISION}"
                --deployment-target "${GDPP_MIN_IOS_VERSION}"
                --schema "${GDPP_NATIVE_SDK_SCHEMA}"
                --runtime-abi "${GDPP_NATIVE_RUNTIME_ABI}"
                --gdpp-version "${PROJECT_VERSION}"
            DEPENDS
                "${CMAKE_SOURCE_DIR}/tools/build_ios_sdk.py"
                "${GDPP_NATIVE_RUNTIME_HEADER}"
                "${GDPP_REFERENCE_SEMANTICS_HEADER}"
                "${GDPP_NATIVE_RUNTIME_SOURCE}"
                "${GDPP_ATTACHED_RUNTIME_HEADER}"
                "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
                "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
                "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
                "${GDPP_INTEGER_SEMANTICS_HEADER}"
            COMMENT "Packaging Godot ${GDPP_IOS_VERSION} iOS/arm64 SDK"
            VERBATIM
        )
        list(APPEND GDPP_IOS_SDK_TARGETS "${GDPP_IOS_TARGET}")
    endforeach()
    add_custom_target(gdpp_ios_sdk DEPENDS ${GDPP_IOS_SDK_TARGETS})
endif()

set(GDPP_API_DATA_FILES)
foreach(GDPP_API_VERSION IN LISTS GDPP_SUPPORTED_GODOT_VERSIONS)
    string(REPLACE "." "_" GDPP_API_SUFFIX "${GDPP_API_VERSION}")
    set(GDPP_API_FILE_VARIABLE "GDPP_GODOT_API_FILE_${GDPP_API_SUFFIX}")
    set(GDPP_API_JSON "${${GDPP_API_FILE_VARIABLE}}")
    set(GDPP_API_DATA
        "${CMAKE_BINARY_DIR}/generated/gdpp/godot_api_data_${GDPP_API_SUFFIX}.inc")
    add_custom_command(
        OUTPUT "${GDPP_API_DATA}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/generate_godot_api.py"
                --input "${GDPP_API_JSON}" --output "${GDPP_API_DATA}"
                --namespace "generated_api_${GDPP_API_SUFFIX}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/generate_godot_api.py" "${GDPP_API_JSON}"
        COMMENT "Generating Godot ${GDPP_API_VERSION} API metadata"
        VERBATIM
    )
    list(APPEND GDPP_API_DATA_FILES "${GDPP_API_DATA}")
endforeach()
add_custom_target(gdpp_api_data DEPENDS ${GDPP_API_DATA_FILES})

# Keep the build manifest aligned with include/gdpp/<module> and src/<module>. The repository
# architecture check enforces cross-module include direction; these named groups make target
# ownership reviewable without introducing per-directory CMake files.
set(GDPP_CORE_SOURCES
    src/core/compiler_stack.cpp
    src/core/diagnostic.cpp
    src/core/godot_version.cpp
    src/core/source.cpp
)
set(GDPP_FRONTEND_SOURCES
    src/frontend/ast_serializer.cpp
    src/frontend/constant_evaluator.cpp
    src/frontend/language_features.cpp
    src/frontend/lexer.cpp
    src/frontend/literal.cpp
    src/frontend/parser.cpp
    src/frontend/token.cpp
    src/frontend/unicode.cpp
)
set(GDPP_SEMANTIC_SOURCES
    src/semantic/analyzer.cpp
    src/semantic/flow.cpp
    src/semantic/godot_api.cpp
    src/semantic/intrinsics.cpp
    src/semantic/iteration.cpp
    src/semantic/script_symbols.cpp
    src/semantic/type.cpp
    src/semantic/conversion.cpp
)
set(GDPP_IR_SOURCES
    src/ir/lowering.cpp
    src/ir/mir.cpp
    src/ir/mir_optimizer.cpp
    src/ir/mir_serializer.cpp
    src/ir/optimizer.cpp
)
set(GDPP_CODEGEN_SOURCES
    src/codegen/cpp_generator.cpp
)
set(GDPP_COMPILER_SOURCES
    src/compiler/compiler.cpp
)
set(GDPP_PROJECT_SOURCES
    src/project/extension_bridge.cpp
    src/project/native_builder.cpp
    src/project/project_compiler.cpp
    src/project/project_file_selector.cpp
)
set(GDPP_SUPPORT_SOURCES
    src/support/sha256.cpp
)

find_package(Threads REQUIRED)
add_library(
    gdpp_core
    ${GDPP_CORE_SOURCES}
    ${GDPP_FRONTEND_SOURCES}
    ${GDPP_SEMANTIC_SOURCES}
    ${GDPP_IR_SOURCES}
    ${GDPP_CODEGEN_SOURCES}
    ${GDPP_COMPILER_SOURCES}
    ${GDPP_PROJECT_SOURCES}
    ${GDPP_SUPPORT_SOURCES}
)
add_dependencies(gdpp_core gdpp_api_data)
target_link_libraries(gdpp_core PRIVATE Threads::Threads)
set_source_files_properties(
    src/semantic/godot_api.cpp
    PROPERTIES OBJECT_DEPENDS "${GDPP_API_DATA_FILES}"
)
add_library(gdpp::core ALIAS gdpp_core)
target_include_directories(
    gdpp_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>"
)
target_compile_features(gdpp_core PUBLIC cxx_std_17)
set_target_properties(gdpp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
gdpp_set_project_warnings(gdpp_core)

if(GDPP_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
        message(FATAL_ERROR "GDPP_BUILD_FUZZERS requires Clang with libFuzzer support")
    endif()
    add_library(
        gdpp_frontend_fuzz_core
        STATIC
        ${GDPP_CORE_SOURCES}
        ${GDPP_FRONTEND_SOURCES}
    )
    target_include_directories(
        gdpp_frontend_fuzz_core
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>"
    )
    target_compile_features(gdpp_frontend_fuzz_core PUBLIC cxx_std_17)
    target_compile_options(
        gdpp_frontend_fuzz_core
        PRIVATE
            -fsanitize=fuzzer-no-link,address,undefined
            -fno-sanitize-recover=undefined
            -fno-omit-frame-pointer
    )
    gdpp_set_project_warnings(gdpp_frontend_fuzz_core)

    add_executable(gdpp_frontend_fuzzer test/fuzz/frontend_fuzzer.cpp)
    target_link_libraries(gdpp_frontend_fuzzer PRIVATE gdpp_frontend_fuzz_core)
    target_compile_options(
        gdpp_frontend_fuzzer
        PRIVATE
            -fsanitize=fuzzer,address,undefined
            -fno-sanitize-recover=undefined
            -fno-omit-frame-pointer
    )
    target_link_options(
        gdpp_frontend_fuzzer
        PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
    )
    gdpp_set_project_warnings(gdpp_frontend_fuzzer)
    file(COPY "${CMAKE_SOURCE_DIR}/test/fuzz/corpus/frontend"
         DESTINATION "${CMAKE_BINARY_DIR}/fuzz-corpus")
endif()

if(GDPP_BUILD_CLI)
    add_executable(gdpp src/cli/main.cpp)
    target_link_libraries(gdpp PRIVATE gdpp::core)
    gdpp_set_project_warnings(gdpp)
endif()

if(GDPP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(test)
endif()

if(GDPP_BUILD_EXTENSION)
    include(GodotPlugin)
endif()
