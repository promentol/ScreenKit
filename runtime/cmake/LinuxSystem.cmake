# The system libraries the runtime links on Linux, as imported targets named the
# way the Apple prebuilts name theirs: sdl3::sdl3, sdl3::ttf, gles::gles
# (EGL and GLES from the GPU driver -- Mesa on a Raspberry Pi -- where Apple
# links ANGLE), and openssl::openssl (the image's TLS, which is the whole of
# what makes https work on this platform).
#
# On Linux these are shared with the system rather than shipped
# (Architecture.md 11): a Batocera image already carries SDL3 and SDL3_ttf, and
# the GLES that matches its GPU. Two ways to find them:
#
#   pkg-config            a build on a distribution with the -dev packages
#   SCREENKIT_SYSROOT     a directory with include/ and lib/ holding the target
#                         system's own libraries -- how tools/batocera links
#                         against a Batocera image's SDL3, which has neither
#                         headers nor a compiler on the device
#
# GLES and EGL headers come from the build machine either way (Khronos headers
# are the same everywhere); the libraries are resolved by soname on the device.

set(SCREENKIT_SYSROOT "" CACHE PATH
    "include/ and lib/ holding the target system's SDL3 and SDL3_ttf (instead of pkg-config)")

if(SCREENKIT_SYSROOT)
  foreach(_lib SDL3 SDL3_ttf)
    find_library(${_lib}_LIBRARY NAMES ${_lib} PATHS "${SCREENKIT_SYSROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  endforeach()
  if(NOT EXISTS "${SCREENKIT_SYSROOT}/include/SDL3/SDL.h" OR NOT EXISTS "${SCREENKIT_SYSROOT}/include/SDL3_ttf/SDL_ttf.h")
    message(FATAL_ERROR "SCREENKIT_SYSROOT=${SCREENKIT_SYSROOT} has no include/SDL3/SDL.h and include/SDL3_ttf/SDL_ttf.h")
  endif()
  add_library(sdl3::sdl3 SHARED IMPORTED GLOBAL)
  set_target_properties(sdl3::sdl3 PROPERTIES
    IMPORTED_LOCATION "${SDL3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include"
    # Libraries copied off the device depend on others this machine does not
    # have (Wayland, libdrm, FreeType...). The device resolves those at run time.
    INTERFACE_LINK_OPTIONS "LINKER:--unresolved-symbols=ignore-in-shared-libs")
  add_library(sdl3::ttf SHARED IMPORTED GLOBAL)
  set_target_properties(sdl3::ttf PROPERTIES
    IMPORTED_LOCATION "${SDL3_ttf_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include")
  target_link_libraries(sdl3::ttf INTERFACE sdl3::sdl3)
  message(STATUS "SDL3 and SDL3_ttf from ${SCREENKIT_SYSROOT}")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET GLOBAL sdl3>=3.2)
  pkg_check_modules(SDL3_TTF REQUIRED IMPORTED_TARGET GLOBAL sdl3-ttf>=3.2)
  add_library(sdl3::sdl3 ALIAS PkgConfig::SDL3)
  add_library(sdl3::ttf ALIAS PkgConfig::SDL3_TTF)
  message(STATUS "SDL3 ${SDL3_VERSION} and SDL3_ttf ${SDL3_TTF_VERSION} from pkg-config")
endif()

find_path(GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)
find_path(EGL_INCLUDE_DIR EGL/egl.h REQUIRED)
find_library(GLESV2_LIBRARY NAMES GLESv2 REQUIRED)
find_library(EGL_LIBRARY NAMES EGL REQUIRED)
add_library(gles::gles INTERFACE IMPORTED GLOBAL)
set_target_properties(gles::gles PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${GLES3_INCLUDE_DIR};${EGL_INCLUDE_DIR}"
  INTERFACE_LINK_LIBRARIES "${GLESV2_LIBRARY};${EGL_LIBRARY}")
message(STATUS "GLES: ${GLESV2_LIBRARY}")

# ---- OpenSSL: the image's TLS -------------------------------------------------
#
# The Linux HTTP client (core/src/net/NetServiceLinux.cpp) is cpp-httplib built
# with CPPHTTPLIB_OPENSSL_SUPPORT, and the TLS under it is the distribution's
# own -- no bundled library and no CA roots in this repo (Architecture.md 10.3).
# So an image without libssl has no https at all, and that has to be a loud
# failure here rather than a runtime surprise or, worse, a silent fall back to
# plaintext. Everything below is REQUIRED for that reason.
#
# Libraries from the target, headers from the build machine -- the same split
# GLES and EGL already use above, and for a stronger reason: OpenSSL's
# `opensslconf.h` and `configuration.h` are *generated* by its Configure script
# and are not in the source release at all, so there is no tarball to take
# matching headers from the way tools/batocera/pi.sh does for SDL. `pi.sh
# sysroot` copies the device's libssl/libcrypto and refuses a soname this
# arrangement cannot serve; see tools/batocera/README.md.
if(SCREENKIT_SYSROOT)
  foreach(_ssl ssl crypto)
    find_library(OPENSSL_${_ssl}_LIBRARY NAMES ${_ssl}
                 PATHS "${SCREENKIT_SYSROOT}/lib" NO_DEFAULT_PATH)
    if(NOT OPENSSL_${_ssl}_LIBRARY)
      message(FATAL_ERROR
        "no lib${_ssl} in ${SCREENKIT_SYSROOT}/lib -- the Linux HTTP client is cpp-httplib over the "
        "image's OpenSSL, and without it there is no TLS and no https.\n"
        "  Run: sh tools/batocera/pi.sh sysroot   (it copies libssl and libcrypto off the device)\n"
        "  If the device itself has no OpenSSL, this image cannot run the runtime's networking.")
    endif()
  endforeach()
  find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h)
  if(NOT OPENSSL_INCLUDE_DIR)
    message(FATAL_ERROR
      "no openssl/ssl.h on this build machine -- the headers come from here, the libraries from "
      "the image (see the comment above).\n"
      "  In the tools/batocera build container: apt-get install libssl-dev")
  endif()
  add_library(openssl::openssl INTERFACE IMPORTED GLOBAL)
  set_target_properties(openssl::openssl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${OPENSSL_ssl_LIBRARY};${OPENSSL_crypto_LIBRARY}")
  if(EXISTS "${SCREENKIT_SYSROOT}/openssl-version.txt")
    file(READ "${SCREENKIT_SYSROOT}/openssl-version.txt" _openssl_version)
    string(STRIP "${_openssl_version}" _openssl_version)
    message(STATUS "OpenSSL from ${SCREENKIT_SYSROOT} (device: ${_openssl_version})")
  else()
    message(STATUS "OpenSSL from ${SCREENKIT_SYSROOT}")
  endif()
else()
  # 3.0 is the floor: `X509_STORE_load_locations` and `SSL_CTX_set_cert_store`
  # are older than that, but 1.1.1 has been out of support since 2023 and
  # nothing this runtime targets still ships it.
  find_package(OpenSSL 3.0 REQUIRED)
  add_library(openssl::openssl INTERFACE IMPORTED GLOBAL)
  set_target_properties(openssl::openssl PROPERTIES
    INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto")
  message(STATUS "OpenSSL ${OPENSSL_VERSION} from the system")
endif()

# ---- the media stack: libvlc, Wayland, libdrm, GBM ------------------------------
#
# The <video> player on Linux (core/src/media/MediaPlayerLinux.cpp) is the
# image's own libvlc, whose frames it presents on a Wayland subsurface through
# dma-bufs -- and like everything else here it links the image's own libraries
# dynamically and ships none of them (spec-video-player.md). Imported targets:
#
#   vlc::vlc           libvlc (the player's API)
#   vlc::plugin        libvlccore and VLC's plugin headers: the ScreenKit decoder
#                      plugin (core/src/media/linux/vlc-plugin/) is a VLC module
#   ffmpeg44::avcodec  libavcodec and libavutil -- the FFmpeg set *VLC* links (4.4 on
#                      Batocera 42), whose *_v4l2m2m decoders the plugin drives
#   wayland::client    libwayland-client
#   libdrm::drm        libdrm
#   gbm::gbm           Mesa's GBM (linear buffer objects for the frames)
#
# plus SCREENKIT_WAYLAND_SCANNER and SCREENKIT_WAYLAND_PROTOCOLS_DIR, which the
# media block in runtime/CMakeLists.txt runs over linux-dmabuf-v1, viewporter and
# presentation-time. REQUIRED, like OpenSSL: a Linux runtime whose <video> could
# not play would be a quiet regression, so a machine without the libraries fails
# here, naming what to install.
#
# With SCREENKIT_SYSROOT (tools/batocera): the device's libraries and the headers
# of the releases it runs, both put there by `pi.sh sysroot` (media-headers.sh),
# and the scanner that release builds. Otherwise pkg-config and the build
# machine's own scanner and protocols.
if(SCREENKIT_SYSROOT)
  foreach(_lib vlc vlccore avcodec avutil wayland-client drm gbm)
    string(MAKE_C_IDENTIFIER "${_lib}" _var)
    find_library(MEDIA_${_var}_LIBRARY NAMES ${_lib} PATHS "${SCREENKIT_SYSROOT}/lib" NO_DEFAULT_PATH)
    if(NOT MEDIA_${_var}_LIBRARY)
      set(_media_missing TRUE)
    endif()
  endforeach()
  set(SCREENKIT_WAYLAND_SCANNER "${SCREENKIT_SYSROOT}/bin/wayland-scanner")
  set(SCREENKIT_WAYLAND_PROTOCOLS_DIR "${SCREENKIT_SYSROOT}/share/wayland-protocols")
  if(_media_missing OR NOT EXISTS "${SCREENKIT_SYSROOT}/include/vlc/vlc.h"
     OR NOT EXISTS "${SCREENKIT_SYSROOT}/include/vlc/plugins/vlc_codec.h"
     OR NOT EXISTS "${SCREENKIT_SYSROOT}/include/ffmpeg4.4/libavcodec/avcodec.h"
     OR NOT EXISTS "${SCREENKIT_SYSROOT}/include/wayland-client.h" OR NOT EXISTS "${SCREENKIT_WAYLAND_SCANNER}")
    message(FATAL_ERROR
      "SCREENKIT_SYSROOT=${SCREENKIT_SYSROOT} has no media stack (libvlc and VLC's plugin headers, the FFmpeg "
      "4.4 libavcodec VLC links, libwayland-client, libdrm, GBM and their headers) -- the <video> player links "
      "the image's own.\n"
      "  Run: sh tools/batocera/pi.sh sysroot")
  endif()
  add_library(vlc::vlc INTERFACE IMPORTED GLOBAL)
  set_target_properties(vlc::vlc PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include"
    INTERFACE_LINK_LIBRARIES "${MEDIA_vlc_LIBRARY}")
  add_library(vlc::plugin INTERFACE IMPORTED GLOBAL)
  set_target_properties(vlc::plugin PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include/vlc/plugins"
    INTERFACE_LINK_LIBRARIES "${MEDIA_vlccore_LIBRARY}")
  add_library(ffmpeg44::avcodec INTERFACE IMPORTED GLOBAL)
  set_target_properties(ffmpeg44::avcodec PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include/ffmpeg4.4"
    INTERFACE_LINK_LIBRARIES "${MEDIA_avcodec_LIBRARY};${MEDIA_avutil_LIBRARY}"
    # libavcodec off the device depends on codec libraries this machine does not
    # have; the device resolves them at run time.
    INTERFACE_LINK_OPTIONS "LINKER:--unresolved-symbols=ignore-in-shared-libs")
  add_library(wayland::client INTERFACE IMPORTED GLOBAL)
  set_target_properties(wayland::client PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include"
    INTERFACE_LINK_LIBRARIES "${MEDIA_wayland_client_LIBRARY}")
  add_library(libdrm::drm INTERFACE IMPORTED GLOBAL)
  set_target_properties(libdrm::drm PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include;${SCREENKIT_SYSROOT}/include/libdrm"
    INTERFACE_LINK_LIBRARIES "${MEDIA_drm_LIBRARY}")
  add_library(gbm::gbm INTERFACE IMPORTED GLOBAL)
  set_target_properties(gbm::gbm PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SCREENKIT_SYSROOT}/include"
    INTERFACE_LINK_LIBRARIES "${MEDIA_gbm_LIBRARY}")
  if(EXISTS "${SCREENKIT_SYSROOT}/media-versions.txt")
    file(READ "${SCREENKIT_SYSROOT}/media-versions.txt" _media_versions)
    string(STRIP "${_media_versions}" _media_versions)
    message(STATUS "media stack from ${SCREENKIT_SYSROOT}: ${_media_versions}")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBVLC IMPORTED_TARGET GLOBAL libvlc>=3.0)
  pkg_check_modules(VLC_PLUGIN IMPORTED_TARGET GLOBAL vlc-plugin>=3.0)
  pkg_check_modules(AVCODEC IMPORTED_TARGET GLOBAL libavcodec libavutil)
  pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET GLOBAL wayland-client)
  pkg_check_modules(LIBDRM IMPORTED_TARGET GLOBAL libdrm)
  pkg_check_modules(GBM IMPORTED_TARGET GLOBAL gbm)
  pkg_get_variable(SCREENKIT_WAYLAND_PROTOCOLS_PKGDATA wayland-protocols pkgdatadir)
  find_program(SCREENKIT_WAYLAND_SCANNER wayland-scanner)
  if(NOT LIBVLC_FOUND OR NOT VLC_PLUGIN_FOUND OR NOT AVCODEC_FOUND OR NOT WAYLAND_CLIENT_FOUND OR NOT LIBDRM_FOUND
     OR NOT GBM_FOUND OR NOT SCREENKIT_WAYLAND_PROTOCOLS_PKGDATA OR NOT SCREENKIT_WAYLAND_SCANNER)
    message(FATAL_ERROR
      "the <video> player needs libvlc 3 and VLC's plugin headers, the libavcodec VLC links, "
      "libwayland-client, wayland-scanner, wayland-protocols, libdrm and GBM, from pkg-config.\n"
      "  Debian/Ubuntu: apt-get install libvlc-dev libvlccore-dev libavcodec-dev libwayland-dev "
      "wayland-protocols libdrm-dev libgbm-dev")
  endif()
  # The same file names the sysroot uses, from the protocols' own layout.
  set(SCREENKIT_WAYLAND_PROTOCOLS_DIR "${CMAKE_BINARY_DIR}/wayland-protocol-xml")
  file(MAKE_DIRECTORY "${SCREENKIT_WAYLAND_PROTOCOLS_DIR}")
  foreach(_xml linux-dmabuf/linux-dmabuf-v1 viewporter/viewporter presentation-time/presentation-time)
    get_filename_component(_name "${_xml}" NAME)
    configure_file("${SCREENKIT_WAYLAND_PROTOCOLS_PKGDATA}/stable/${_xml}.xml"
                   "${SCREENKIT_WAYLAND_PROTOCOLS_DIR}/${_name}.xml" COPYONLY)
  endforeach()
  add_library(vlc::vlc ALIAS PkgConfig::LIBVLC)
  add_library(vlc::plugin ALIAS PkgConfig::VLC_PLUGIN)
  add_library(ffmpeg44::avcodec ALIAS PkgConfig::AVCODEC)
  add_library(wayland::client ALIAS PkgConfig::WAYLAND_CLIENT)
  add_library(libdrm::drm ALIAS PkgConfig::LIBDRM)
  add_library(gbm::gbm ALIAS PkgConfig::GBM)
  message(STATUS "media stack from pkg-config: libvlc ${LIBVLC_VERSION}, libavcodec ${AVCODEC_libavcodec_VERSION}, "
                 "wayland-client ${WAYLAND_CLIENT_VERSION}")
endif()
