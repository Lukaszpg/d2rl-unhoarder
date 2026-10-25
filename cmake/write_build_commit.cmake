# Executed by the build, not only during CMake configure. Never read or
# rewrite repository sources: this generates a header exclusively in build/.
if(NOT DEFINED UNHOARDER_SOURCE_ROOT OR
   NOT DEFINED UNHOARDER_OUTPUT_DIR)
    message(FATAL_ERROR "UnHoarder source and output directories are required")
endif()

set(commit "unknown")
find_program(UNHOARDER_GIT_EXECUTABLE NAMES git)
if(UNHOARDER_GIT_EXECUTABLE)
    execute_process(
        COMMAND "${UNHOARDER_GIT_EXECUTABLE}" -C "${UNHOARDER_SOURCE_ROOT}"
                rev-parse --verify HEAD
        RESULT_VARIABLE gitResult
        OUTPUT_VARIABLE gitHash
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if("${gitResult}" STREQUAL "0" AND gitHash MATCHES "^[0-9a-fA-F]+$")
        string(LENGTH "${gitHash}" length)
        if(length EQUAL 40 OR length EQUAL 64)
            string(SUBSTRING "${gitHash}" 0 12 commit)
        endif()
    endif()
endif()

file(MAKE_DIRECTORY "${UNHOARDER_OUTPUT_DIR}")
set(header "${UNHOARDER_OUTPUT_DIR}/unhoarder_build_commit.hpp")
set(contents "#pragma once\n#define UNHOARDER_BUILD_COMMIT_STRING \"${commit}\"\n")
if(EXISTS "${header}")
    file(READ "${header}" existing)
    if(existing STREQUAL contents)
        return()
    endif()
endif()
file(WRITE "${header}" "${contents}")
