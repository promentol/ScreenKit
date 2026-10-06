# The vendored cpp-httplib header, as an imported target -- and the check that
# it is still the file that was pinned.
#
# third_party/httplib/ is one MIT header taken whole from one upstream tag
# (its VENDOR.md). Vendored code is never hand-edited, and the only way to hold
# that rule is to check it. The check itself lives in tools/vendor/httplib.sh,
# which owns the pin: re-implementing the comparison here would be a second
# copy of the sha256s to keep in step with the first.

# CMAKE_CURRENT_LIST_DIR, not CMAKE_CURRENT_SOURCE_DIR: this file knows where it
# is, and resolving through the including directory silently ties it to one
# include site.
get_filename_component(SCREENKIT_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SCREENKIT_HTTPLIB_DIR "${SCREENKIT_RUNTIME_DIR}/third_party/httplib")
set(SCREENKIT_HTTPLIB_SCRIPT "${SCREENKIT_RUNTIME_DIR}/../tools/vendor/httplib.sh")

execute_process(
  COMMAND sh "${SCREENKIT_HTTPLIB_SCRIPT}" --check
  RESULT_VARIABLE _httplib_rc
  OUTPUT_VARIABLE _httplib_out
  ERROR_VARIABLE _httplib_err)
if(NOT _httplib_rc EQUAL 0)
  message(FATAL_ERROR
    "the vendored cpp-httplib is not what tools/vendor/httplib.sh pinned:\n"
    "${_httplib_err}${_httplib_out}"
    "  Vendored code is never hand-edited (third_party/httplib/VENDOR.md).\n"
    "  Run: sh tools/vendor/httplib.sh")
endif()

# A changed pin, or an edited header, must reconfigure rather than be believed
# from a stale cache.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${SCREENKIT_HTTPLIB_DIR}/httplib.h"
             "${SCREENKIT_HTTPLIB_DIR}/LICENSE"
             "${SCREENKIT_HTTPLIB_SCRIPT}")

add_library(httplib::httplib INTERFACE IMPORTED GLOBAL)
set_target_properties(httplib::httplib PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_HTTPLIB_DIR}")
string(STRIP "${_httplib_out}" _httplib_out)
message(STATUS "${_httplib_out}")
