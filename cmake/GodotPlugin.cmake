if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third/godot-cpp/CMakeLists.txt")
    message(FATAL_ERROR "godot-cpp is missing; run git submodule update --init --recursive")
endif()

set(GODOTCPP_API_VERSION "${GDPP_GODOT_API_VERSION}" CACHE STRING "" FORCE)
set(GODOTCPP_TARGET editor CACHE STRING "" FORCE)
set(GODOTCPP_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "" FORCE)
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/third/godot-cpp"
    "${CMAKE_BINARY_DIR}/third/godot-cpp"
    EXCLUDE_FROM_ALL
)

include(ExternalProject)

# Every nested CMake build consumes a precompiled godot-cpp archive and therefore belongs to the
# same native ABI domain as the parent. Keep the toolchain-bearing cache variables in one list so
# SDK profile builds and independent provider fixtures cannot silently drift apart.
function(gdpp_collect_native_cmake_contract output_variable)
    set(contract_arguments)
    foreach(contract_variable IN ITEMS
            CMAKE_CXX_COMPILER
            CMAKE_TOOLCHAIN_FILE
            CMAKE_SYSROOT
            CMAKE_CXX_COMPILER_TARGET
            CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
            CMAKE_OSX_DEPLOYMENT_TARGET
            CMAKE_OSX_SYSROOT
            CMAKE_MSVC_RUNTIME_LIBRARY
            CMAKE_RC_COMPILER
            CMAKE_MT)
        if(DEFINED ${contract_variable} AND NOT "${${contract_variable}}" STREQUAL "")
            string(REPLACE ";" "\\;" contract_value "${${contract_variable}}")
            list(APPEND contract_arguments "-D${contract_variable}=${contract_value}")
        endif()
    endforeach()
    set(${output_variable} "${contract_arguments}" PARENT_SCOPE)
endfunction()

gdpp_collect_native_cmake_contract(GDPP_NATIVE_CMAKE_CONTRACT_ARGS)

function(gdpp_add_sdk_binding target_name api_version godot_target output_variable)
    set(build_directory "${CMAKE_BINARY_DIR}/sdk/${target_name}")

    # Distribution profiles share one optimized godot-cpp binding. Script-level Debug behavior is
    # controlled independently by GDPP, so shipping a second template_debug archive would add
    # hundreds of megabytes without changing the GDExtension ABI used by exported games.
    if(godot_target STREQUAL "template_release")
        set(binding_build_type Release)
        set(binding_build_config Release)
    elseif(CMAKE_CONFIGURATION_TYPES)
        set(binding_build_type "$<CONFIG>")
        set(binding_build_config "$<CONFIG>")
    else()
        set(binding_build_type "${CMAKE_BUILD_TYPE}")
        set(binding_build_config "${CMAKE_BUILD_TYPE}")
    endif()
    if(binding_build_type STREQUAL "")
        set(binding_build_type Debug)
        set(binding_build_config Debug)
    endif()
    set(configure_arguments
        -DCMAKE_BUILD_TYPE=${binding_build_type}
        -DGODOTCPP_API_VERSION=${api_version}
        -DGODOTCPP_TARGET=${godot_target}
        -DGODOTCPP_ENABLE_TESTING=OFF
        -DGODOTCPP_SYSTEM_HEADERS=ON
        ${GDPP_NATIVE_CMAKE_CONTRACT_ARGS}
    )
    if(CMAKE_OSX_ARCHITECTURES)
        string(REPLACE ";" "|" escaped_osx_architectures "${CMAKE_OSX_ARCHITECTURES}")
        list(APPEND configure_arguments
            "-DCMAKE_OSX_ARCHITECTURES=${escaped_osx_architectures}")
    endif()
    if(MSVC)
        list(APPEND configure_arguments
            "-DCMAKE_CXX_FLAGS=/FS /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00")
    elseif(WIN32)
        list(APPEND configure_arguments
            "-DCMAKE_CXX_FLAGS=-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
    endif()
    set(generator_arguments)
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND generator_arguments CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND generator_arguments CMAKE_GENERATOR_TOOLSET "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    ExternalProject_Add(
        ${target_name}
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/third/godot-cpp"
        BINARY_DIR "${build_directory}"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        ${generator_arguments}
        LIST_SEPARATOR "|"
        CMAKE_ARGS ${configure_arguments}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
                      --config "${binding_build_config}" --target godot-cpp --parallel
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
    )
    set(${output_variable} "${build_directory}" PARENT_SCOPE)
endfunction()

set(GDPP_SDK_BINDING_TARGETS)
foreach(GDPP_SDK_VERSION IN LISTS GDPP_PACKAGE_GODOT_VERSIONS)
    string(REPLACE "." "_" GDPP_SDK_SUFFIX "${GDPP_SDK_VERSION}")
    set(GDPP_SDK_DEPENDENCY_VARIABLE "GDPP_SDK_BINDING_TARGETS_${GDPP_SDK_SUFFIX}")
    set(${GDPP_SDK_DEPENDENCY_VARIABLE})
    set(GDPP_RELEASE_TARGET "gdpp_godot_cpp_${GDPP_SDK_SUFFIX}_release")
    gdpp_add_sdk_binding(
        "${GDPP_RELEASE_TARGET}"
        "${GDPP_SDK_VERSION}"
        template_release
        "GDPP_SDK_RELEASE_BUILD_${GDPP_SDK_SUFFIX}"
    )
    list(APPEND GDPP_SDK_BINDING_TARGETS "${GDPP_RELEASE_TARGET}")
    list(APPEND ${GDPP_SDK_DEPENDENCY_VARIABLE} "${GDPP_RELEASE_TARGET}")
endforeach()

# Each entry above is a complete godot-cpp build and its nested build already uses all available
# cores. Starting several variants at once multiplies compiler memory/handle pressure without
# improving useful throughput; on MSVC this has produced orphaned build forests and dependency DB
# lock failures after one parent build exits. Keep variants sequential by default, including the
# in-tree editor binding, but leave parallelism inside each individual build untouched.
if(GDPP_SERIALIZE_SDK_BINDING_BUILDS)
    set(GDPP_PREVIOUS_SDK_BINDING_TARGET godot-cpp)
    foreach(GDPP_SDK_BINDING_TARGET IN LISTS GDPP_SDK_BINDING_TARGETS)
        add_dependencies(
            "${GDPP_SDK_BINDING_TARGET}"
            "${GDPP_PREVIOUS_SDK_BINDING_TARGET}"
        )
        set(GDPP_PREVIOUS_SDK_BINDING_TARGET "${GDPP_SDK_BINDING_TARGET}")
    endforeach()
endif()

add_library(gdpp_runtime STATIC
    "${CMAKE_SOURCE_DIR}/src/runtime/variant_ops.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/attached_script_registry.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/attached_script_instance.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/attached_script_language.cpp"
)
add_library(gdpp::runtime ALIAS gdpp_runtime)
target_include_directories(gdpp_runtime PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(gdpp_runtime PUBLIC godot::cpp)
target_compile_features(gdpp_runtime PUBLIC cxx_std_17)
set_target_properties(gdpp_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
gdpp_set_project_warnings(gdpp_runtime)

string(TOLOWER "${CMAKE_SYSTEM_NAME}" GDPP_PLATFORM)
if(GDPP_PLATFORM STREQUAL "darwin")
    set(GDPP_PLATFORM "macos")
elseif(GDPP_PLATFORM STREQUAL "emscripten")
    set(GDPP_PLATFORM "web")
endif()
if(GDPP_PLATFORM STREQUAL "windows")
    set(GDPP_PLATFORM_MINIMUM "Windows_10")
elseif(GDPP_PLATFORM STREQUAL "macos")
    set(GDPP_PLATFORM_MINIMUM "macOS_11.0")
elseif(GDPP_PLATFORM STREQUAL "linux")
    set(GDPP_PLATFORM_MINIMUM "Ubuntu_22.04")
else()
    set(GDPP_PLATFORM_MINIMUM "none")
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" GDPP_ARCH)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES GDPP_OSX_ARCHITECTURE_COUNT)
    if(GDPP_OSX_ARCHITECTURE_COUNT EQUAL 2 AND
            "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES AND
            "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(GDPP_ARCH "universal")
    elseif(GDPP_OSX_ARCHITECTURE_COUNT EQUAL 1 AND
            "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(GDPP_ARCH "arm64")
    elseif(GDPP_OSX_ARCHITECTURE_COUNT EQUAL 1 AND
            "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(GDPP_ARCH "x86_64")
    else()
        message(FATAL_ERROR
            "Unsupported macOS architecture set: ${CMAKE_OSX_ARCHITECTURES}")
    endif()
elseif(GDPP_ARCH MATCHES "^(aarch64|arm64)$")
    set(GDPP_ARCH "arm64")
elseif(GDPP_ARCH MATCHES "^(amd64|x86_64)$")
    set(GDPP_ARCH "x86_64")
endif()

set(GDPP_EXAMPLE_DIRECTORY "${CMAKE_SOURCE_DIR}/example")
set(GDPP_ADDON_DIRECTORY "${GDPP_EXAMPLE_DIRECTORY}/addons/gdpp")
set(GDPP_EXTENSION_OUTPUT "${GDPP_ADDON_DIRECTORY}/binary")
# A generator expression prevents multi-config generators from silently appending Debug/Release
# below the install-ready add-on directory.
set(GDPP_EXTENSION_OUTPUT_FLAT "${GDPP_EXTENSION_OUTPUT}/$<0:>")
set(GDPP_PROJECT_BUILD_ROOT "${GDPP_ADDON_DIRECTORY}/build")
file(MAKE_DIRECTORY
    "${GDPP_ADDON_DIRECTORY}"
    "${GDPP_PROJECT_BUILD_ROOT}"
    "${GDPP_EXAMPLE_DIRECTORY}/.godot"
)

add_library(
    gdpp_godot_plugin
    SHARED
    "${CMAKE_SOURCE_DIR}/src/integration/godot/compiler_service.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/godot/register_types.cpp"
)
find_package(Threads REQUIRED)
target_link_libraries(
    gdpp_godot_plugin
    PRIVATE gdpp::core gdpp::runtime godot::cpp Threads::Threads
)
if(WIN32)
    target_link_libraries(gdpp_godot_plugin PRIVATE user32)
endif()
target_include_directories(
    gdpp_godot_plugin
    PRIVATE "${CMAKE_SOURCE_DIR}/src/integration/godot"
)
target_compile_features(gdpp_godot_plugin PRIVATE cxx_std_17)
set_target_properties(gdpp_godot_plugin PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
if(UNIX AND NOT APPLE)
    # godot-cpp may statically link libstdc++/libgcc for portable Linux bundles. Hide every
    # archive symbol inside this DSO: otherwise a second project GDExtension can interpose its
    # C++ runtime/locale facets and corrupt std::filesystem/string destruction in the compiler.
    target_link_options(gdpp_godot_plugin PRIVATE "LINKER:--exclude-libs,ALL")
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/generated/gdpp/compiler.exports.map"
        CONTENT "{ global: gdpp_library_init; local: *; };\n"
    )
    target_link_options(gdpp_godot_plugin PRIVATE
        "LINKER:--version-script=${CMAKE_BINARY_DIR}/generated/gdpp/compiler.exports.map")
elseif(APPLE)
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/generated/gdpp/compiler.exports.macos"
        CONTENT "_gdpp_library_init\n"
    )
    target_link_options(gdpp_godot_plugin PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_BINARY_DIR}/generated/gdpp/compiler.exports.macos")
endif()
if(APPLE)
    target_link_options(gdpp_godot_plugin PRIVATE
        "$<$<CONFIG:Release>:LINKER:-dead_strip>"
        "$<$<CONFIG:Release>:LINKER:-x>")
elseif(UNIX)
    target_link_options(gdpp_godot_plugin PRIVATE "$<$<CONFIG:Release>:LINKER:-s>")
elseif(MSVC)
    target_link_options(gdpp_godot_plugin PRIVATE
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF>"
        "$<$<CONFIG:Release>:/INCREMENTAL:NO>")
endif()
target_compile_definitions(
    gdpp_godot_plugin
    PRIVATE
        GDPP_SDK_ROOT=""
        GDPP_PLATFORM="${GDPP_PLATFORM}"
        GDPP_ARCH="${GDPP_ARCH}"
)
gdpp_set_project_warnings(gdpp_godot_plugin)

set_target_properties(
    gdpp_godot_plugin
    PROPERTIES
        OUTPUT_NAME "gdpp_compiler.${GDPP_PLATFORM}.${GDPP_ARCH}"
        LIBRARY_OUTPUT_DIRECTORY "${GDPP_EXTENSION_OUTPUT_FLAT}"
        RUNTIME_OUTPUT_DIRECTORY "${GDPP_EXTENSION_OUTPUT_FLAT}"
        ARCHIVE_OUTPUT_DIRECTORY "${GDPP_EXTENSION_OUTPUT_FLAT}"
)

if(GDPP_BUILD_TESTS)
    set(GDPP_ATTACHED_TEST_ROOT "${CMAKE_BINARY_DIR}/attached-extension-project")
    set(GDPP_ATTACHED_TEST_VENDOR_OUTPUT "${CMAKE_BINARY_DIR}/attached-extension-vendor")
    set(GDPP_ATTACHED_TEST_GENERATED "${CMAKE_BINARY_DIR}/attached-extension-generated")
    add_library(
        gdpp_test_vendor
        SHARED
        "${CMAKE_SOURCE_DIR}/test/attached_extension/vendor_base.cpp"
        "${CMAKE_SOURCE_DIR}/test/attached_extension/register_types.cpp"
    )
    target_include_directories(
        gdpp_test_vendor
        PRIVATE "${CMAKE_SOURCE_DIR}/test/attached_extension"
    )
    target_link_libraries(gdpp_test_vendor PRIVATE godot::cpp)
    target_compile_features(gdpp_test_vendor PRIVATE cxx_std_17)
    set_target_properties(
        gdpp_test_vendor
        PROPERTIES
            OUTPUT_NAME "gdpp_test_vendor.${GDPP_PLATFORM}.${GDPP_ARCH}"
            LIBRARY_OUTPUT_DIRECTORY "${GDPP_ATTACHED_TEST_VENDOR_OUTPUT}"
            RUNTIME_OUTPUT_DIRECTORY "${GDPP_ATTACHED_TEST_VENDOR_OUTPUT}"
            ARCHIVE_OUTPUT_DIRECTORY "${GDPP_ATTACHED_TEST_VENDOR_OUTPUT}"
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
    )
    if(UNIX AND NOT APPLE)
        target_link_options(gdpp_test_vendor PRIVATE "LINKER:--exclude-libs,ALL")
        file(GENERATE
            OUTPUT "${GDPP_ATTACHED_TEST_GENERATED}/vendor.exports.map"
            CONTENT "{ global: gdpp_test_vendor_init; local: *; };\n"
        )
        target_link_options(gdpp_test_vendor PRIVATE
            "LINKER:--version-script=${GDPP_ATTACHED_TEST_GENERATED}/vendor.exports.map")
    elseif(APPLE)
        file(GENERATE
            OUTPUT "${GDPP_ATTACHED_TEST_GENERATED}/vendor.exports.macos"
            CONTENT "_gdpp_test_vendor_init\n"
        )
        target_link_options(gdpp_test_vendor PRIVATE
            "LINKER:-exported_symbols_list,${GDPP_ATTACHED_TEST_GENERATED}/vendor.exports.macos")
    endif()
    gdpp_set_project_warnings(gdpp_test_vendor)

    if(APPLE AND GDPP_ARCH STREQUAL "universal")
        string(CONCAT GDPP_ATTACHED_TEST_VENDOR_LIBRARIES
            "macos.editor.arm64 = \"res://addons/vendor/binary/"
            "$<TARGET_FILE_NAME:gdpp_test_vendor>\"\n"
            "macos.editor.x86_64 = \"res://addons/vendor/binary/"
            "$<TARGET_FILE_NAME:gdpp_test_vendor>\"\n"
            "macos.release.arm64 = \"res://addons/vendor/binary/"
            "${CMAKE_SHARED_LIBRARY_PREFIX}gdpp_test_vendor.release.macos.universal"
            "${CMAKE_SHARED_LIBRARY_SUFFIX}\"\n"
            "macos.release.x86_64 = \"res://addons/vendor/binary/"
            "${CMAKE_SHARED_LIBRARY_PREFIX}gdpp_test_vendor.release.macos.universal"
            "${CMAKE_SHARED_LIBRARY_SUFFIX}\"")
    else()
        string(CONCAT GDPP_ATTACHED_TEST_VENDOR_LIBRARIES
            "${GDPP_PLATFORM}.editor.${GDPP_ARCH} = \"res://addons/vendor/binary/"
            "$<TARGET_FILE_NAME:gdpp_test_vendor>\"\n"
            "${GDPP_PLATFORM}.release.${GDPP_ARCH} = \"res://addons/vendor/binary/"
            "${CMAKE_SHARED_LIBRARY_PREFIX}gdpp_test_vendor.release.${GDPP_PLATFORM}."
            "${GDPP_ARCH}${CMAKE_SHARED_LIBRARY_SUFFIX}\"")
    endif()
    file(GENERATE
        OUTPUT "${GDPP_ATTACHED_TEST_GENERATED}/vendor.gdextension"
        CONTENT
            "[configuration]\n\nentry_symbol = \"gdpp_test_vendor_init\"\ncompatibility_minimum = \"${GDPP_GODOT_API_VERSION}\"\nreloadable = false\n\n[libraries]\n\n${GDPP_ATTACHED_TEST_VENDOR_LIBRARIES}\n"
    )
endif()

# Integration tests must load the compiler produced by the current build, never a stale
# multi-platform package artifact that happens to remain in the example project. Include a
# build-tree identity because several architecture/configuration trees may coexist and CMake
# configure must not silently rewrite another tree's descriptor.
string(SHA256 GDPP_TEST_BUILD_ID "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${GDPP_TEST_BUILD_ID}" 0 12 GDPP_TEST_BUILD_ID)
set(GDPP_TEST_COMPILER_DESCRIPTOR_NAME
    "compiler_test.${GDPP_PLATFORM}.${GDPP_ARCH}.${GDPP_TEST_BUILD_ID}.gdextension")
# Older build trees placed their test-only descriptors below res://addons. Godot discovers every
# .gdextension there before the explicit registry is applied, which can load the compiler twice
# and corrupt ClassDB registration. Remove only that retired generated filename family during
# configure, then keep the active descriptor inside Godot's non-scanned metadata directory.
file(GLOB GDPP_LEGACY_TEST_COMPILER_DESCRIPTORS
    "${GDPP_PROJECT_BUILD_ROOT}/compiler_test.*.gdextension")
if(GDPP_LEGACY_TEST_COMPILER_DESCRIPTORS)
    file(REMOVE ${GDPP_LEGACY_TEST_COMPILER_DESCRIPTORS})
endif()
set(GDPP_TEST_COMPILER_DESCRIPTOR
    "${GDPP_EXAMPLE_DIRECTORY}/.godot/${GDPP_TEST_COMPILER_DESCRIPTOR_NAME}")
set(GDPP_TEST_COMPILER_DESCRIPTOR_RESOURCE
    "res://.godot/${GDPP_TEST_COMPILER_DESCRIPTOR_NAME}")
if(APPLE AND GDPP_ARCH STREQUAL "universal")
    string(CONCAT GDPP_TEST_COMPILER_LIBRARIES
        "macos.editor.arm64 = \"res://addons/gdpp/binary/$<TARGET_FILE_NAME:gdpp_godot_plugin>\"\n"
        "macos.editor.x86_64 = \"res://addons/gdpp/binary/$<TARGET_FILE_NAME:gdpp_godot_plugin>\"")
else()
    set(GDPP_TEST_COMPILER_LIBRARIES
        "${GDPP_PLATFORM}.editor.${GDPP_ARCH} = \"res://addons/gdpp/binary/$<TARGET_FILE_NAME:gdpp_godot_plugin>\"")
endif()
file(GENERATE
    OUTPUT "${GDPP_TEST_COMPILER_DESCRIPTOR}"
    CONTENT
        "[configuration]\n\nentry_symbol = \"gdpp_library_init\"\ncompatibility_minimum = \"4.4\"\nreloadable = false\n\n[libraries]\n\n${GDPP_TEST_COMPILER_LIBRARIES}\n"
)

# A tiny no-op runtime keeps ordinary GDScript exports runnable when an AOT export preflight fails.
# On a successful export the export plugin replaces the stable descriptor with the project library.
add_library(
    gdpp_fallback
    SHARED
    "${CMAKE_SOURCE_DIR}/src/integration/godot/export_fallback.cpp"
)
target_link_libraries(gdpp_fallback PRIVATE godot::cpp)
target_compile_features(gdpp_fallback PRIVATE cxx_std_17)
set_target_properties(gdpp_fallback PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
if(UNIX AND NOT APPLE)
    target_link_options(gdpp_fallback PRIVATE "LINKER:--exclude-libs,ALL")
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/generated/gdpp/fallback.exports.map"
        CONTENT "{ global: gdpp_export_fallback_library_init; local: *; };\n"
    )
    target_link_options(gdpp_fallback PRIVATE
        "LINKER:--version-script=${CMAKE_BINARY_DIR}/generated/gdpp/fallback.exports.map")
elseif(APPLE)
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/generated/gdpp/fallback.exports.macos"
        CONTENT "_gdpp_export_fallback_library_init\n"
    )
    target_link_options(gdpp_fallback PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_BINARY_DIR}/generated/gdpp/fallback.exports.macos")
endif()
if(APPLE)
    target_link_options(gdpp_fallback PRIVATE
        "$<$<CONFIG:Release>:LINKER:-dead_strip>"
        "$<$<CONFIG:Release>:LINKER:-x>")
elseif(UNIX)
    target_link_options(gdpp_fallback PRIVATE "$<$<CONFIG:Release>:LINKER:-s>")
elseif(MSVC)
    target_link_options(gdpp_fallback PRIVATE
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF>"
        "$<$<CONFIG:Release>:/INCREMENTAL:NO>")
endif()
set_target_properties(
    gdpp_fallback
    PROPERTIES
        OUTPUT_NAME "gdpp_fallback.${GDPP_PLATFORM}.${GDPP_ARCH}"
        LIBRARY_OUTPUT_DIRECTORY "${GDPP_EXTENSION_OUTPUT_FLAT}"
        RUNTIME_OUTPUT_DIRECTORY "${GDPP_EXTENSION_OUTPUT_FLAT}"
        ARCHIVE_OUTPUT_DIRECTORY "${GDPP_EXTENSION_OUTPUT_FLAT}"
)
gdpp_set_project_warnings(gdpp_fallback)

set(GDPP_PACKAGED_SDK "${GDPP_ADDON_DIRECTORY}/sdk")
set(GDPP_PACKAGED_SDK_TARGETS)
if(GDPP_PLATFORM STREQUAL "windows")
    set(GDPP_SDK_MSVC_RUNTIME "static")
else()
    set(GDPP_SDK_MSVC_RUNTIME "not_applicable")
endif()
foreach(GDPP_SDK_VERSION IN LISTS GDPP_PACKAGE_GODOT_VERSIONS)
    string(REPLACE "." "_" GDPP_SDK_SUFFIX "${GDPP_SDK_VERSION}")
    set(GDPP_SDK_DIRECTORY "${GDPP_PACKAGED_SDK}/${GDPP_SDK_VERSION}")
    set(GDPP_PACKAGED_SDK_${GDPP_SDK_SUFFIX} "${GDPP_SDK_DIRECTORY}")
    set(GDPP_SDK_MANIFEST
        "${CMAKE_BINARY_DIR}/generated/gdpp/sdk-${GDPP_SDK_VERSION}.manifest")
    file(GENERATE
        OUTPUT "${GDPP_SDK_MANIFEST}"
        CONTENT
            "GDPP_SDK ${GDPP_NATIVE_SDK_SCHEMA}\napi ${GDPP_SDK_VERSION}\nplatform ${GDPP_PLATFORM}\narch ${GDPP_ARCH}\nprofiles debug,release\ndistribution_binding template_release\ndistribution_optimization Release\nplatform_minimum ${GDPP_PLATFORM_MINIMUM}\ngdpp_version ${PROJECT_VERSION}\ncxx_standard 17\nexceptions disabled\nmsvc_runtime ${GDPP_SDK_MSVC_RUNTIME}\nruntime_abi ${GDPP_NATIVE_RUNTIME_ABI}\nruntime_header_sha256 ${GDPP_NATIVE_RUNTIME_HEADER_SHA256}\nreference_semantics_header_sha256 ${GDPP_REFERENCE_SEMANTICS_HEADER_SHA256}\nruntime_source_sha256 ${GDPP_NATIVE_RUNTIME_SOURCE_SHA256}\nattached_runtime_header_sha256 ${GDPP_ATTACHED_RUNTIME_HEADER_SHA256}\nattached_runtime_registry_source_sha256 ${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE_SHA256}\nattached_runtime_instance_source_sha256 ${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE_SHA256}\nattached_runtime_language_source_sha256 ${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE_SHA256}\ninteger_semantics_header_sha256 ${GDPP_INTEGER_SEMANTICS_HEADER_SHA256}\ncompiler ${CMAKE_CXX_COMPILER_ID}\ncompiler_version ${CMAKE_CXX_COMPILER_VERSION}\n"
    )

    set(GDPP_SDK_PACKAGE_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${GDPP_SDK_DIRECTORY}/include/gdpp/runtime"
                "${GDPP_SDK_DIRECTORY}/include/gdpp/numeric"
                "${GDPP_SDK_DIRECTORY}/src/runtime"
                "${GDPP_SDK_DIRECTORY}/godot-cpp/gen"
                "${GDPP_SDK_DIRECTORY}/lib"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_NATIVE_RUNTIME_HEADER}"
                "${GDPP_SDK_DIRECTORY}/include/gdpp/runtime/variant_ops.hpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_REFERENCE_SEMANTICS_HEADER}"
                "${GDPP_SDK_DIRECTORY}/include/gdpp/runtime/reference_semantics.hpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_NATIVE_RUNTIME_SOURCE}"
                "${GDPP_SDK_DIRECTORY}/src/runtime/variant_ops.cpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ATTACHED_RUNTIME_HEADER}"
                "${GDPP_SDK_DIRECTORY}/include/gdpp/runtime/attached_script.hpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
                "${GDPP_SDK_DIRECTORY}/src/runtime/attached_script_registry.cpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
                "${GDPP_SDK_DIRECTORY}/src/runtime/attached_script_instance.cpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
                "${GDPP_SDK_DIRECTORY}/src/runtime/attached_script_language.cpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_INTEGER_SEMANTICS_HEADER}"
                "${GDPP_SDK_DIRECTORY}/include/gdpp/numeric/integer_semantics.hpp"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/third/godot-cpp/include"
                "${GDPP_SDK_DIRECTORY}/godot-cpp/include"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/third/godot-cpp/LICENSE.md"
                "${GDPP_SDK_DIRECTORY}/godot-cpp/LICENSE.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${GDPP_SDK_RELEASE_BUILD_${GDPP_SDK_SUFFIX}}/gen/include"
                "${GDPP_SDK_DIRECTORY}/godot-cpp/gen/include"
    )
    # godot-cpp deliberately fixes PREFIX to "lib" on every host, including MSVC where CMake's
    # default static-library prefix is empty. Read the upstream target property so external SDK
    # builds and the in-tree binding always use the same archive basename.
    get_target_property(GDPP_GODOT_CPP_LIBRARY_PREFIX godot-cpp PREFIX)
    if(NOT GDPP_GODOT_CPP_LIBRARY_PREFIX)
        message(FATAL_ERROR "godot-cpp must declare its static-library PREFIX")
    endif()
    set(GDPP_RELEASE_LIBRARY_NAME
        "${GDPP_GODOT_CPP_LIBRARY_PREFIX}godot-cpp.${GDPP_PLATFORM}.template_release.${GDPP_ARCH}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND GDPP_SDK_PACKAGE_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_SDK_RELEASE_BUILD_${GDPP_SDK_SUFFIX}}/bin/${GDPP_RELEASE_LIBRARY_NAME}"
                "${GDPP_SDK_DIRECTORY}/lib/${GDPP_RELEASE_LIBRARY_NAME}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_SDK_MANIFEST}"
                "${GDPP_SDK_DIRECTORY}/sdk.manifest"
    )
    set(GDPP_SDK_DEPENDENCY_VARIABLE "GDPP_SDK_BINDING_TARGETS_${GDPP_SDK_SUFFIX}")
    set(GDPP_PACKAGED_SDK_TARGET "gdpp_packaged_sdk_${GDPP_SDK_SUFFIX}")
    add_custom_target(
        ${GDPP_PACKAGED_SDK_TARGET}
        # Host SDK rebuilds and independently installed target packs share the version root.
        # Remove only host-owned paths so packaging macOS/Windows/Linux can never erase Android.
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${GDPP_SDK_DIRECTORY}/include"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${GDPP_SDK_DIRECTORY}/src"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${GDPP_SDK_DIRECTORY}/godot-cpp"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${GDPP_SDK_DIRECTORY}/lib"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${GDPP_SDK_DIRECTORY}/sdk.manifest"
        ${GDPP_SDK_PACKAGE_COMMANDS}
        DEPENDS
            ${${GDPP_SDK_DEPENDENCY_VARIABLE}}
            "${GDPP_SDK_MANIFEST}"
            "${CMAKE_SOURCE_DIR}/third/godot-cpp/LICENSE.md"
            "${GDPP_NATIVE_RUNTIME_HEADER}"
            "${GDPP_NATIVE_RUNTIME_SOURCE}"
            "${GDPP_ATTACHED_RUNTIME_HEADER}"
            "${GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE}"
            "${GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE}"
            "${GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE}"
            "${GDPP_INTEGER_SEMANTICS_HEADER}"
        COMMENT "Packaging compiler-only Godot ${GDPP_SDK_VERSION} SDK"
        VERBATIM
    )
    list(APPEND GDPP_PACKAGED_SDK_TARGETS "${GDPP_PACKAGED_SDK_TARGET}")
endforeach()

add_custom_target(
    gdpp_packaged_sdk ALL
    DEPENDS ${GDPP_PACKAGED_SDK_TARGETS}
)

if(GDPP_BUILD_TESTS)
    list(GET GDPP_PACKAGE_GODOT_VERSIONS 0 GDPP_ATTACHED_TEST_SDK_VERSION)
    string(REPLACE "." "_" GDPP_ATTACHED_TEST_SDK_SUFFIX
        "${GDPP_ATTACHED_TEST_SDK_VERSION}")
    set(GDPP_ATTACHED_TEST_SDK
        "${GDPP_PACKAGED_SDK_${GDPP_ATTACHED_TEST_SDK_SUFFIX}}")
    set(GDPP_ATTACHED_TEST_RELEASE_BUILD
        "${CMAKE_BINARY_DIR}/attached-extension-vendor-release-build")
    string(CONCAT GDPP_ATTACHED_TEST_RELEASE_LIBRARY
        "${GDPP_ATTACHED_TEST_VENDOR_OUTPUT}/${CMAKE_SHARED_LIBRARY_PREFIX}"
        "gdpp_test_vendor.release.${GDPP_PLATFORM}.${GDPP_ARCH}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(GDPP_ATTACHED_TEST_RELEASE_CONFIGURE_ARGS
        -S "${CMAKE_SOURCE_DIR}/test/attached_extension/provider_release"
        -B "${GDPP_ATTACHED_TEST_RELEASE_BUILD}"
        -G "${CMAKE_GENERATOR}"
        -DCMAKE_BUILD_TYPE=Release
        ${GDPP_NATIVE_CMAKE_CONTRACT_ARGS}
        -DGDPP_SDK_DIR=${GDPP_ATTACHED_TEST_SDK}
        -DGDPP_VENDOR_SOURCE_DIR=${CMAKE_SOURCE_DIR}/test/attached_extension
        -DGDPP_OUTPUT_DIR=${GDPP_ATTACHED_TEST_VENDOR_OUTPUT}
        -DGDPP_PLATFORM=${GDPP_PLATFORM}
        -DGDPP_ARCH=${GDPP_ARCH}
    )
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND GDPP_ATTACHED_TEST_RELEASE_CONFIGURE_ARGS
            -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND GDPP_ATTACHED_TEST_RELEASE_CONFIGURE_ARGS
            -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
        string(REPLACE ";" "\\;" GDPP_ATTACHED_TEST_OSX_ARCHITECTURES
            "${CMAKE_OSX_ARCHITECTURES}")
        list(APPEND GDPP_ATTACHED_TEST_RELEASE_CONFIGURE_ARGS
            "-DCMAKE_OSX_ARCHITECTURES=${GDPP_ATTACHED_TEST_OSX_ARCHITECTURES}")
    endif()
    add_custom_target(
        gdpp_test_vendor_release
        COMMAND "${CMAKE_COMMAND}" ${GDPP_ATTACHED_TEST_RELEASE_CONFIGURE_ARGS}
        COMMAND "${CMAKE_COMMAND}" --build "${GDPP_ATTACHED_TEST_RELEASE_BUILD}"
                --config Release --parallel
        DEPENDS
            gdpp_packaged_sdk
            "${CMAKE_SOURCE_DIR}/test/attached_extension/provider_release/CMakeLists.txt"
            "${CMAKE_SOURCE_DIR}/test/attached_extension/vendor_base.cpp"
            "${CMAKE_SOURCE_DIR}/test/attached_extension/vendor_base.hpp"
            "${CMAKE_SOURCE_DIR}/test/attached_extension/register_types.cpp"
        COMMENT "Building independent template_release vendor GDExtension"
        VERBATIM
    )
endif()

set(GDPP_SMOKE_DIR "${CMAKE_BINARY_DIR}/generated-smoke")
add_custom_command(
    OUTPUT
        "${GDPP_SMOKE_DIR}/hello_aot.gd.hpp"
        "${GDPP_SMOKE_DIR}/hello_aot.gd.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GDPP_SMOKE_DIR}"
    COMMAND $<TARGET_FILE:gdpp> compile "${CMAKE_SOURCE_DIR}/example/hello.gd"
            --output "${GDPP_SMOKE_DIR}"
    DEPENDS gdpp "${CMAKE_SOURCE_DIR}/example/hello.gd"
    VERBATIM
)
add_library(
    gdpp_generated_smoke
    STATIC
    "${GDPP_SMOKE_DIR}/hello_aot.gd.cpp"
)
target_include_directories(gdpp_generated_smoke PRIVATE "${GDPP_SMOKE_DIR}")
target_link_libraries(gdpp_generated_smoke PRIVATE gdpp::runtime godot::cpp)
target_compile_features(gdpp_generated_smoke PRIVATE cxx_std_17)
gdpp_set_project_warnings(gdpp_generated_smoke)

add_custom_target(
    gdpp_addon ALL
    COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${GDPP_EXTENSION_OUTPUT}/libgdpp.${GDPP_PLATFORM}.editor.${GDPP_ARCH}.dylib"
            "${GDPP_EXTENSION_OUTPUT}/libgdpp.${GDPP_PLATFORM}.editor.${GDPP_ARCH}.so"
            "${GDPP_EXTENSION_OUTPUT}/gdpp.${GDPP_PLATFORM}.editor.${GDPP_ARCH}.dll"
            "${GDPP_EXTENSION_OUTPUT}/libgdpp.${GDPP_PLATFORM}.template_release.${GDPP_ARCH}.dylib"
            "${GDPP_EXTENSION_OUTPUT}/libgdpp.${GDPP_PLATFORM}.template_release.${GDPP_ARCH}.so"
            "${GDPP_EXTENSION_OUTPUT}/gdpp.${GDPP_PLATFORM}.template_release.${GDPP_ARCH}.dll"
    DEPENDS gdpp_godot_plugin gdpp_fallback gdpp_packaged_sdk
    VERBATIM
)

if(GDPP_BUILD_TESTS)
    add_custom_target(
        gdpp_attached_extension_fixture ALL
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${GDPP_ATTACHED_TEST_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/test/attached_extension/project"
                "${GDPP_ATTACHED_TEST_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${GDPP_ATTACHED_TEST_ROOT}/.godot"
                "${GDPP_ATTACHED_TEST_ROOT}/export"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/vendor/binary"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/binary"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ATTACHED_TEST_ROOT}/extension_list.compiler.cfg"
                "${GDPP_ATTACHED_TEST_ROOT}/.godot/extension_list.cfg"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ATTACHED_TEST_GENERATED}/vendor.gdextension"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/vendor/vendor.gdextension"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:gdpp_test_vendor>"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/vendor/binary/$<TARGET_FILE_NAME:gdpp_test_vendor>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ATTACHED_TEST_RELEASE_LIBRARY}"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/vendor/binary"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_TEST_COMPILER_DESCRIPTOR}"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/gdpp.gdextension"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ADDON_DIRECTORY}/plugin.cfg"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/plugin.cfg"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ADDON_DIRECTORY}/plugin.gd"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/plugin.gd"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ADDON_DIRECTORY}/export_plugin.gd"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/export_plugin.gd"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ADDON_DIRECTORY}/build_progress.gd"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/build_progress.gd"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDPP_ADDON_DIRECTORY}/native_build_job.gd"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/native_build_job.gd"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:gdpp_godot_plugin>"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/binary/$<TARGET_FILE_NAME:gdpp_godot_plugin>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/sdk/${GDPP_ATTACHED_TEST_SDK_VERSION}"
        COMMAND "${CMAKE_COMMAND}" -E touch
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/sdk/.gdignore"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${GDPP_ATTACHED_TEST_SDK}"
                "${GDPP_ATTACHED_TEST_ROOT}/addons/gdpp/sdk/${GDPP_ATTACHED_TEST_SDK_VERSION}"
        DEPENDS gdpp_addon gdpp_test_vendor gdpp_test_vendor_release
        COMMENT "Preparing independent GDExtension attachment integration project"
        VERBATIM
    )
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/test/godot/smoke.gd"
    "${GDPP_PROJECT_BUILD_ROOT}/service_smoke.gd"
    COPYONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/test/godot/direct_export_build.gd"
    "${GDPP_PROJECT_BUILD_ROOT}/direct_export_build.gd"
    COPYONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/test/godot/toolchain_execution.gd"
    "${GDPP_PROJECT_BUILD_ROOT}/toolchain_execution.gd"
    COPYONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/test/godot/build_progress_model.gd"
    "${GDPP_PROJECT_BUILD_ROOT}/build_progress_model.gd"
    COPYONLY
)
if(GDPP_BUILD_TESTS AND EXISTS "${GDPP_GODOT_EXECUTABLE}")
    set(GDPP_GODOT_TEST_LOG_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/godot-test-logs")
    file(MAKE_DIRECTORY "${GDPP_GODOT_TEST_LOG_DIRECTORY}")
    add_test(
        NAME gdpp.godot.reset_extension_registry
        COMMAND "${CMAKE_COMMAND}"
                -DGDPP_PROJECT_DIRECTORY=${GDPP_EXAMPLE_DIRECTORY}
                -DGDPP_COMPILER_DESCRIPTOR=${GDPP_TEST_COMPILER_DESCRIPTOR_RESOURCE}
                -P "${CMAKE_SOURCE_DIR}/test/native_project/write_extension_registry.cmake"
    )
    set_tests_properties(
        gdpp.godot.reset_extension_registry
        PROPERTIES FIXTURES_SETUP gdpp_clean_extension_registry
    )
    add_test(
        NAME gdpp.godot.restore_extension_registry
        COMMAND "${CMAKE_COMMAND}"
                -DGDPP_PROJECT_DIRECTORY=${GDPP_EXAMPLE_DIRECTORY}
                -DGDPP_COMPILER_DESCRIPTOR=res://addons/gdpp/gdpp.gdextension
                -P "${CMAKE_SOURCE_DIR}/test/native_project/write_extension_registry.cmake"
    )
    set_tests_properties(
        gdpp.godot.restore_extension_registry
        PROPERTIES FIXTURES_CLEANUP gdpp_clean_extension_registry
    )
    add_test(
        NAME gdpp.godot.service
        COMMAND "${GDPP_GODOT_EXECUTABLE}" --headless --path "${GDPP_EXAMPLE_DIRECTORY}"
                --log-file "${GDPP_GODOT_TEST_LOG_DIRECTORY}/service.log"
                --script addons/gdpp/build/service_smoke.gd
    )
    set_tests_properties(
        gdpp.godot.service
        PROPERTIES
            PASS_REGULAR_EXPRESSION "GDPP_SMOKE_OK"
            FIXTURES_REQUIRED gdpp_clean_extension_registry
            RESOURCE_LOCK gdpp_godot_user_data
            TIMEOUT 60
    )
    add_test(
        NAME gdpp.godot.toolchain_execution
        COMMAND "${GDPP_GODOT_EXECUTABLE}" --headless --path "${GDPP_EXAMPLE_DIRECTORY}"
                --log-file "${GDPP_GODOT_TEST_LOG_DIRECTORY}/toolchain-execution.log"
                --script addons/gdpp/build/toolchain_execution.gd
    )
    set_tests_properties(
        gdpp.godot.toolchain_execution
        PROPERTIES
            PASS_REGULAR_EXPRESSION "GDPP_TOOLCHAIN_EXECUTION_OK"
            FIXTURES_REQUIRED gdpp_clean_extension_registry
            RESOURCE_LOCK gdpp_godot_user_data
            TIMEOUT 60
    )
    add_test(
        NAME gdpp.godot.build_progress_model
        COMMAND "${GDPP_GODOT_EXECUTABLE}" --headless --path "${GDPP_EXAMPLE_DIRECTORY}"
                --log-file "${GDPP_GODOT_TEST_LOG_DIRECTORY}/build-progress-model.log"
                --script addons/gdpp/build/build_progress_model.gd
    )
    set_tests_properties(
        gdpp.godot.build_progress_model
        PROPERTIES
            PASS_REGULAR_EXPRESSION "GDPP_BUILD_PROGRESS_MODEL_OK"
            FIXTURES_REQUIRED gdpp_clean_extension_registry
            RESOURCE_LOCK gdpp_godot_user_data
            TIMEOUT 60
    )
    add_test(
        NAME gdpp.godot.direct_export_build
        COMMAND "${GDPP_GODOT_EXECUTABLE}" --headless --path "${GDPP_EXAMPLE_DIRECTORY}"
                --log-file "${GDPP_GODOT_TEST_LOG_DIRECTORY}/direct-export-build.log"
                --script addons/gdpp/build/direct_export_build.gd
    )
    set_tests_properties(
        gdpp.godot.direct_export_build
        PROPERTIES
            PASS_REGULAR_EXPRESSION "GDPP_DIRECT_EXPORT_BUILD_OK"
            FIXTURES_REQUIRED gdpp_clean_extension_registry
            RESOURCE_LOCK gdpp_godot_user_data
            TIMEOUT 600
    )
endif()
