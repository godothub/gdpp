if(NOT DEFINED GDPP_TEST_BINARY_DIR OR NOT DEFINED GDPP_TEST_SDK_VERSIONS OR
        NOT DEFINED GDPP_TEST_ADDON_DIR OR NOT DEFINED GDPP_TEST_SOURCE_DIR OR
        NOT DEFINED GDPP_TEST_PRECISION OR NOT DEFINED GDPP_TEST_CONFIG)
    message(FATAL_ERROR "SDK profile contract test requires the build directory and SDK versions")
endif()

if(GDPP_TEST_CONFIG STREQUAL "")
    set(GDPP_TEST_CONFIG Debug)
endif()

foreach(GDPP_TEST_SDK_VERSION IN LISTS GDPP_TEST_SDK_VERSIONS)
    string(REPLACE "." "_" GDPP_TEST_SDK_SUFFIX "${GDPP_TEST_SDK_VERSION}")
    set(GDPP_TEST_CACHE
        "${GDPP_TEST_BINARY_DIR}/sdk/gdpp_godot_cpp_${GDPP_TEST_SDK_SUFFIX}_release/CMakeCache.txt")
    if(NOT EXISTS "${GDPP_TEST_CACHE}")
        message(FATAL_ERROR "Packaged Release SDK cache is missing: ${GDPP_TEST_CACHE}")
    endif()

    file(STRINGS "${GDPP_TEST_CACHE}" GDPP_TEST_BUILD_TYPE_LINE
        REGEX "^CMAKE_BUILD_TYPE:STRING=")
    file(STRINGS "${GDPP_TEST_CACHE}" GDPP_TEST_CONFIGURATION_TYPES_LINE
        REGEX "^CMAKE_CONFIGURATION_TYPES:STRING=")
    file(STRINGS "${GDPP_TEST_CACHE}" GDPP_TEST_PRECISION_LINE
        REGEX "^GODOTCPP_PRECISION:STRING=")
    string(REPLACE "\\;" ";" GDPP_TEST_CONFIGURATION_TYPES
        "${GDPP_TEST_CONFIGURATION_TYPES_LINE}")
    if(NOT GDPP_TEST_BUILD_TYPE_LINE STREQUAL "CMAKE_BUILD_TYPE:STRING=Release" AND
            NOT GDPP_TEST_CONFIGURATION_TYPES MATCHES "(^|;)Release(;|$)")
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} distribution binding cannot produce Release; "
            "single-config='${GDPP_TEST_BUILD_TYPE_LINE}', "
            "multi-config='${GDPP_TEST_CONFIGURATION_TYPES}'")
    endif()
    if(NOT GDPP_TEST_PRECISION_LINE STREQUAL
            "GODOTCPP_PRECISION:STRING=${GDPP_TEST_PRECISION}")
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} binding precision differs from the GDPP package")
    endif()

    set(GDPP_TEST_SDK_ROOT "${GDPP_TEST_ADDON_DIR}/sdk/${GDPP_TEST_SDK_VERSION}")
    file(GLOB GDPP_TEST_BINDINGS LIST_DIRECTORIES false "${GDPP_TEST_SDK_ROOT}/lib/*")
    list(LENGTH GDPP_TEST_BINDINGS GDPP_TEST_BINDING_COUNT)
    if(NOT GDPP_TEST_BINDING_COUNT EQUAL 1)
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} host SDK must contain exactly one "
            "template_release binding, found: ${GDPP_TEST_BINDINGS}")
    endif()
    set(GDPP_TEST_RELEASE_BINDINGS ${GDPP_TEST_BINDINGS})
    list(FILTER GDPP_TEST_RELEASE_BINDINGS INCLUDE REGEX "\\.template_release\\.")
    list(LENGTH GDPP_TEST_RELEASE_BINDINGS GDPP_TEST_RELEASE_BINDING_COUNT)
    if(NOT GDPP_TEST_RELEASE_BINDING_COUNT EQUAL 1)
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} host SDK must contain exactly one template_release "
            "binding")
    endif()

    file(STRINGS "${GDPP_TEST_SDK_ROOT}/sdk.manifest" GDPP_TEST_DISTRIBUTION_BINDING_LINE
        REGEX "^distribution_binding ")
    if(NOT GDPP_TEST_DISTRIBUTION_BINDING_LINE STREQUAL
            "distribution_binding template_release")
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} SDK has an invalid distribution binding contract")
    endif()
    file(STRINGS "${GDPP_TEST_SDK_ROOT}/sdk.manifest" GDPP_TEST_API_KIND_LINE
        REGEX "^api_kind ")
    file(STRINGS "${GDPP_TEST_SDK_ROOT}/sdk.manifest" GDPP_TEST_API_SHA256_LINE
        REGEX "^api_sha256 ")
    file(STRINGS "${GDPP_TEST_SDK_ROOT}/sdk.manifest" GDPP_TEST_MANIFEST_PRECISION_LINE
        REGEX "^precision ")
    if(NOT GDPP_TEST_API_KIND_LINE STREQUAL "api_kind official")
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} release SDK must use the official API corpus")
    endif()
    if(NOT GDPP_TEST_MANIFEST_PRECISION_LINE STREQUAL
            "precision ${GDPP_TEST_PRECISION}")
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} SDK precision manifest is inconsistent")
    endif()
    if(GDPP_TEST_SDK_VERSION STREQUAL "4.7")
        set(GDPP_TEST_API_FILE
            "${GDPP_TEST_SOURCE_DIR}/third/godot-cpp/gdextension/extension_api.json")
    else()
        string(REPLACE "." "-" GDPP_TEST_API_DASHED "${GDPP_TEST_SDK_VERSION}")
        set(GDPP_TEST_API_FILE
            "${GDPP_TEST_SOURCE_DIR}/third/godot-cpp/gdextension/extension_api-${GDPP_TEST_API_DASHED}.json")
    endif()
    file(SHA256 "${GDPP_TEST_API_FILE}" GDPP_TEST_API_SHA256)
    if(NOT GDPP_TEST_API_SHA256_LINE STREQUAL
            "api_sha256 ${GDPP_TEST_API_SHA256}")
        message(FATAL_ERROR
            "Godot ${GDPP_TEST_SDK_VERSION} SDK API fingerprint is stale")
    endif()
endforeach()
