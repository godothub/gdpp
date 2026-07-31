include_guard(GLOBAL)

set(GDPP_NINJA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third/ninja")
set(GDPP_NINJA_VERSION "1.13.2")
set(GDPP_NINJA_COMMIT "3441b633c2fe2c494e958780ba0f4227b1327634")

if(NOT EXISTS "${GDPP_NINJA_SOURCE_DIR}/src/ninja.cc" OR
        NOT EXISTS "${GDPP_NINJA_SOURCE_DIR}/COPYING")
    message(FATAL_ERROR
        "The pinned Ninja ${GDPP_NINJA_VERSION} submodule is unavailable. "
        "Run git submodule update --init --recursive.")
endif()

set(GDPP_NINJA_SOURCES
    "${GDPP_NINJA_SOURCE_DIR}/src/build_log.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/build.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/clean.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/clparser.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/debug_flags.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/depfile_parser.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/deps_log.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/disk_interface.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/dyndep_parser.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/dyndep.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/edit_distance.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/elide_middle.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/eval_env.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/graph.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/graphviz.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/jobserver.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/json.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/lexer.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/line_printer.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/manifest_parser.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/metrics.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/missing_deps.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/ninja.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/parser.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/real_command_runner.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/state.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/status_printer.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/string_piece_util.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/util.cc"
    "${GDPP_NINJA_SOURCE_DIR}/src/version.cc"
)

if(WIN32)
    list(APPEND GDPP_NINJA_SOURCES
        "${GDPP_NINJA_SOURCE_DIR}/src/getopt.c"
        "${GDPP_NINJA_SOURCE_DIR}/src/includes_normalize-win32.cc"
        "${GDPP_NINJA_SOURCE_DIR}/src/jobserver-win32.cc"
        "${GDPP_NINJA_SOURCE_DIR}/src/minidump-win32.cc"
        "${GDPP_NINJA_SOURCE_DIR}/src/msvc_helper_main-win32.cc"
        "${GDPP_NINJA_SOURCE_DIR}/src/msvc_helper-win32.cc"
        "${GDPP_NINJA_SOURCE_DIR}/src/subprocess-win32.cc"
        "${GDPP_NINJA_SOURCE_DIR}/windows/ninja.manifest"
    )
    set_source_files_properties(
        "${GDPP_NINJA_SOURCE_DIR}/src/getopt.c"
        PROPERTIES LANGUAGE CXX
    )
else()
    list(APPEND GDPP_NINJA_SOURCES
        "${GDPP_NINJA_SOURCE_DIR}/src/jobserver-posix.cc"
        "${GDPP_NINJA_SOURCE_DIR}/src/subprocess-posix.cc"
    )
endif()

add_executable(gdpp_ninja ${GDPP_NINJA_SOURCES})
target_compile_features(gdpp_ninja PRIVATE cxx_std_17)
set_target_properties(gdpp_ninja PROPERTIES
    OUTPUT_NAME "gdpp-ninja"
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(gdpp_ninja PRIVATE
        /GR-
        /wd4100
        /wd4244
        /wd4267
        /wd4702
        /wd4706
        /Zc:__cplusplus
    )
    target_compile_definitions(gdpp_ninja PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(gdpp_ninja PRIVATE USE_PPOLL=1)
endif()
