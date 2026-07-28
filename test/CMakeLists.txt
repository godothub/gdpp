add_executable(
    gdpp_tests
    main.cpp
    lexer_test.cpp
    frontend_limits_test.cpp
    constant_evaluator_test.cpp
    integer_semantics_test.cpp
    parser_test.cpp
    ast_serializer_test.cpp
    flow_test.cpp
    type_test.cpp
    script_symbols_test.cpp
    iteration_test.cpp
    ir_test.cpp
    mir_test.cpp
    godot_api_test.cpp
    native_builder_test.cpp
    project_compiler_test.cpp
    compiler_test.cpp
)
target_link_libraries(gdpp_tests PRIVATE gdpp::core)
target_include_directories(gdpp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(
    gdpp_tests
    PRIVATE
        GDPP_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        GDPP_TEST_BINARY_DIR="${PROJECT_BINARY_DIR}"
)
gdpp_set_project_warnings(gdpp_tests)

add_test(NAME gdpp.unit COMMAND gdpp_tests)

add_test(
    NAME gdpp.architecture
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_architecture.py"
)

add_test(
    NAME gdpp.documentation
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_docs.py"
)

add_test(
    NAME gdpp.godot_frontend.audit
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/godot_frontend_audit_test.py"
)

add_test(
    NAME gdpp.release.packaging
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/release_packaging_test.py"
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_BINARY_DIR}/test-release-packaging"
)

add_test(
    NAME gdpp.api.contract
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/godot_api_contract_test.py"
        "${PROJECT_SOURCE_DIR}"
)

add_test(
    NAME gdpp.compatibility.fetch_policy
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/compatibility_fetch_test.py"
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_BINARY_DIR}/compatibility-fetch-test"
)

add_test(
    NAME gdpp.compatibility.external_project_e2e_contract
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/external_project_e2e_test.py"
        "${PROJECT_SOURCE_DIR}"
)

add_test(
    NAME gdpp.export.immutability
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/export_immutability_test.py"
)

add_test(
    NAME gdpp.web.browser_smoke
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/web_browser_smoke_test.py"
)

if(NOT WIN32)
    add_test(
        NAME gdpp.delivery.download_retry
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/download_with_retry_test.py"
    )
endif()

add_test(
    NAME gdpp.addon.clean
    COMMAND "${CMAKE_COMMAND}"
            -DGDPP_TEST_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DGDPP_TEST_BINARY_DIR=${PROJECT_BINARY_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/clean_addon_build_test.cmake"
)

add_test(
    NAME gdpp.addon.descriptor
    COMMAND "${CMAKE_COMMAND}"
            -DGDPP_TEST_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/addon_descriptor_test.cmake"
)

if(GDPP_BUILD_EXTENSION)
    add_test(
        NAME gdpp.sdk.profile_contract
        COMMAND "${CMAKE_COMMAND}"
                "-DGDPP_TEST_BINARY_DIR=${PROJECT_BINARY_DIR}"
                "-DGDPP_TEST_SDK_VERSIONS=${GDPP_PACKAGE_GODOT_VERSIONS}"
                "-DGDPP_TEST_ADDON_DIR=${PROJECT_SOURCE_DIR}/example/addons/gdpp"
                "-DGDPP_TEST_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                "-DGDPP_TEST_PRECISION=${GDPP_GODOT_PRECISION}"
                "-DGDPP_TEST_CONFIG=$<CONFIG>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/sdk_profile_contract_test.cmake"
    )

    if(WIN32)
        add_executable(gdpp_windows_dll_load_test windows_dll_load_test.cpp)
        add_dependencies(gdpp_windows_dll_load_test gdpp_godot_plugin)
        target_compile_features(gdpp_windows_dll_load_test PRIVATE cxx_std_17)
        gdpp_set_project_warnings(gdpp_windows_dll_load_test)
        add_test(
            NAME gdpp.windows.compiler_dll_load
            COMMAND gdpp_windows_dll_load_test "$<TARGET_FILE:gdpp_godot_plugin>"
        )
    endif()
endif()

if(GDPP_BUILD_CLI)
    add_test(
        NAME gdpp.cli.compile
        COMMAND gdpp compile "${PROJECT_SOURCE_DIR}/example/hello.gd"
                --output "${PROJECT_BINARY_DIR}/test-output"
    )
    set_tests_properties(
        gdpp.cli.compile
        PROPERTIES
            FIXTURES_SETUP generated_example
    )
    add_test(
        NAME gdpp.cli.no_optimize
        COMMAND gdpp compile "${PROJECT_SOURCE_DIR}/example/hello.gd"
                --output "${PROJECT_BINARY_DIR}/test-output-unoptimized" --no-optimize
    )
    add_test(
        NAME gdpp.cli.compile_4_5
        COMMAND gdpp compile "${PROJECT_SOURCE_DIR}/example/hello.gd"
                --output "${PROJECT_BINARY_DIR}/test-output-4.5" --target-godot 4.5
    )
    add_test(
        NAME gdpp.cli.compile_4_7
        COMMAND gdpp compile "${PROJECT_SOURCE_DIR}/example/hello.gd"
                --output "${PROJECT_BINARY_DIR}/test-output-4.7" --target-godot 4.7
    )
    add_test(
        NAME gdpp.assert.release_preprocess
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/assert_release_preprocess_test.py"
            --compiler "${CMAKE_CXX_COMPILER}"
            --compiler-id "${CMAKE_CXX_COMPILER_ID}"
            --gdpp "$<TARGET_FILE:gdpp>"
            --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assert_release.gd"
            --output "${PROJECT_BINARY_DIR}/assert-release-preprocess"
    )

    if(GDPP_ENABLE_COMPATIBILITY_CORPUS_TESTS)
        set(GDPP_CORPUS_MANIFEST
            "${PROJECT_SOURCE_DIR}/test/compatibility/official_projects.json")
        set(GDPP_CORPUS_DIRECTORY
            "${PROJECT_BINARY_DIR}/compatibility/godot-demo-projects")
        set(GDPP_CORPUS_OUTPUT "${PROJECT_BINARY_DIR}/compatibility/results")
        add_test(
            NAME gdpp.compatibility.fetch_official
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/fetch_compatibility_corpus.py"
                --manifest "${GDPP_CORPUS_MANIFEST}"
                --destination "${GDPP_CORPUS_DIRECTORY}"
                --build-root "${PROJECT_SOURCE_DIR}/build"
        )
        set_tests_properties(
            gdpp.compatibility.fetch_official
            PROPERTIES FIXTURES_SETUP official_compatibility_corpus TIMEOUT 180
        )
        add_test(
            NAME gdpp.compatibility.official_projects
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/run_compatibility_corpus.py"
                --manifest "${GDPP_CORPUS_MANIFEST}"
                --corpus "${GDPP_CORPUS_DIRECTORY}"
                --compiler "$<TARGET_FILE:gdpp>"
                --cxx-compiler "${CMAKE_CXX_COMPILER}"
                --compiler-id "${CMAKE_CXX_COMPILER_ID}"
                --sdk-root "${PROJECT_SOURCE_DIR}"
                --output "${GDPP_CORPUS_OUTPUT}"
                --target-godot 4.7
        )
        set_tests_properties(
            gdpp.compatibility.official_projects
            PROPERTIES FIXTURES_REQUIRED official_compatibility_corpus TIMEOUT 1200
        )

        set(GDPP_PARSER_CORPUS_MANIFEST
            "${PROJECT_SOURCE_DIR}/test/compatibility/godot_parser_4_7.json")
        set(GDPP_PARSER_CORPUS_DIRECTORY
            "${PROJECT_BINARY_DIR}/compatibility/godot-parser-4.7")
        set(GDPP_PARSER_CORPUS_OUTPUT
            "${PROJECT_BINARY_DIR}/compatibility/parser-results")
        set(GDPP_FRONTEND_SNAPSHOT
            "${PROJECT_SOURCE_DIR}/test/compatibility/godot_frontend_4_7.json")
        add_test(
            NAME gdpp.compatibility.fetch_parser_4_7
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/fetch_compatibility_corpus.py"
                --manifest "${GDPP_PARSER_CORPUS_MANIFEST}"
                --destination "${GDPP_PARSER_CORPUS_DIRECTORY}"
                --build-root "${PROJECT_SOURCE_DIR}/build"
        )
        set_tests_properties(
            gdpp.compatibility.fetch_parser_4_7
            PROPERTIES FIXTURES_SETUP official_parser_4_7_corpus TIMEOUT 180
        )
        add_test(
            NAME gdpp.compatibility.parser_4_7
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/run_parser_corpus.py"
                --manifest "${GDPP_PARSER_CORPUS_MANIFEST}"
                --corpus "${GDPP_PARSER_CORPUS_DIRECTORY}"
                --compiler "$<TARGET_FILE:gdpp>"
                --output "${GDPP_PARSER_CORPUS_OUTPUT}"
                --build-root "${PROJECT_SOURCE_DIR}/build"
                --target-godot 4.7
        )
        set_tests_properties(
            gdpp.compatibility.parser_4_7
            PROPERTIES FIXTURES_REQUIRED official_parser_4_7_corpus TIMEOUT 180
        )
        add_test(
            NAME gdpp.compatibility.frontend_snapshot
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/audit_godot_frontend.py"
                verify
                --snapshot "${GDPP_FRONTEND_SNAPSHOT}"
                --checkout "${GDPP_PARSER_CORPUS_DIRECTORY}"
                --language-features
                "${PROJECT_SOURCE_DIR}/src/frontend/language_features.cpp"
                --unicode-table
                "${PROJECT_SOURCE_DIR}/src/frontend/unicode_identifier_ranges.inc"
                --report
                "${PROJECT_BINARY_DIR}/compatibility/frontend-snapshot-diff.json"
        )
        set_tests_properties(
            gdpp.compatibility.frontend_snapshot
            PROPERTIES FIXTURES_REQUIRED official_parser_4_7_corpus TIMEOUT 60
        )
    endif()
endif()

# CMake's configure-time environment does not automatically propagate to CTest.  Keep Python
# imports performed by fixtures from writing __pycache__ into the checked-in tools directory.
get_property(GDPP_CONFIGURED_TESTS DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
if(GDPP_CONFIGURED_TESTS)
    set_tests_properties(
        ${GDPP_CONFIGURED_TESTS}
        PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
endif()
