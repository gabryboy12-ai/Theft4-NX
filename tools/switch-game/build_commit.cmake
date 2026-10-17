# Run 24 (C): writes theft4_build_commit.h with the commit the build comes
# from ("-dirty" with uncommitted changes outside the submodules), for the
# start-up line of game.log. Run at every build (switch-game-commit target);
# the file is rewritten only when the text changes, so main.cpp is recompiled
# only then.
#   cmake -DOUT=<header> -DREPO=<repository root> -P build_commit.cmake
execute_process(COMMAND git -C "${REPO}" rev-parse --short=7 HEAD
    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR commit STREQUAL "")
    set(commit "unknown")
else()
    execute_process(COMMAND git -C "${REPO}" status --porcelain --untracked-files=no
            --ignore-submodules=all
        OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT changes STREQUAL "")
        string(APPEND commit "-dirty")
    endif()
endif()
set(text "#pragma once\n#define THEFT4_BUILD_COMMIT \"${commit}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
