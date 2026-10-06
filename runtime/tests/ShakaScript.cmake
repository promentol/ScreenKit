# @screenkit/shaka is an ES module; the media rows evaluate it as a script. The
# package keeps everything that is not an export above one marker line, so the
# script is that prefix plus a global -- and a moved or renamed marker fails the
# build here rather than leaving the rows testing nothing.
#
#   cmake -DIN=<src/index.js> -DOUT=<shaka-script.js> -P ShakaScript.cmake
file(READ "${IN}" source)
set(marker "// ---- module exports: runtime/tests evaluates everything above this line as a script")
string(FIND "${source}" "${marker}" at)
if(at EQUAL -1)
  message(FATAL_ERROR "${IN}: the module-exports marker is missing:\n  ${marker}")
endif()
string(SUBSTRING "${source}" 0 ${at} script)
file(WRITE "${OUT}" "${script}globalThis.shaka = shaka;\n'screenkit-shaka';\n")
