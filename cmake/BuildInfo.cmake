# Run at every build (the hailstone_build_info target): writes OUT, the build's version line for core/build_info.cpp
# ("0.1.0-alpha (f6a07d7)", "-dirty" with uncommitted changes), the release version alone ("0.1.0-alpha") and the
# build's date (20261123, local time; SOURCE_DATE_EPOCH when set), touching it only when they change.
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE hash
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
if(failed OR hash STREQUAL "")
  set(line "${VERSION}")
else()
  execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${SRC}"
                  OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT changes STREQUAL "")
    set(hash "${hash}-dirty")
  endif()
  set(line "${VERSION} (${hash})")
endif()
string(TIMESTAMP date "%Y%m%d")
set(text "#define HAILSTONE_BUILD_VERSION \"${line}\"\n#define HAILSTONE_RELEASE_VERSION \"${VERSION}\"\n#define HAILSTONE_BUILD_DATE ${date}\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
