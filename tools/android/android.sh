#!/bin/sh
# ScreenKit on Android TV and Fire TV: the toolchain, the APK, and an emulator or
# a device to run it on. Everything goes through adb; ANDROID_SERIAL picks the
# device when more than one is attached.
#
#   sh tools/android/android.sh sdk                   cmdline-tools, NDK r27d and the android-tv arm64 image (API 34)
#   sh tools/android/android.sh avd                   create the screenkit-tv emulator if needed, boot it, wait
#   sh tools/android/android.sh build [app]           debug APK, arm64-v8a + armeabi-v7a, examples/<app> bundled (pixi-hello)
#   sh tools/android/android.sh install               install that APK
#   sh tools/android/android.sh push <app>            examples/<app>/app.skpkg -> the app's files/apps/<app>.skpkg
#   sh tools/android/android.sh run <app> [secs]      push examples/<app> and launch it ("bundled": the APK's own), print its log
#   sh tools/android/android.sh shot <app> [secs]     the same with a frame capture: out/<app>.png, and out/<app>-screen.png
#   sh tools/android/android.sh key <keyevent>...     remote presses: DPAD_DOWN DPAD_CENTER BACK HOME ...
#   sh tools/android/android.sh logs                  follow logcat, tag ScreenKit
#   sh tools/android/android.sh test [row]...         the runtime's net and media rows on the device, over adb reverse
#
#   ANDROID_HOME=~/Library/Android/sdk   SIZE=640x480   ANDROID_SERIAL=emulator-5554
#   MINIFY=1 sh tools/android/android.sh test          the rows against an R8-minified APK
#
# `build` bundles one app into the APK; `run` pushes and launches any other
# without a rebuild. SIZE runs an app at a fixed size scaled to the screen.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tools/android"
OUT="$HERE/out"
PROJECT="$ROOT/runtime/android"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
export ANDROID_HOME="$SDK"

# The toolchain (runtime/README.md, "Android"). The NDK is the one hermes-android
# was built with; the image is the newest android-tv system image with an arm64
# slice that this emulator runs.
CMDLINE_TOOLS_URL=https://dl.google.com/android/repository/commandlinetools-mac_arm64-16111833_latest.zip
CMDLINE_TOOLS_SHA256=58da8f215781cc0208743e88b65ae259780c55e7eb40e50373d377cf369232d6
NDK="ndk;27.3.13750724"
IMAGE="system-images;android-34;android-tv;arm64-v8a"
AVD=screenkit-tv

APP_ID=dev.screenkit.host
ACTIVITY="$APP_ID/.ScreenKitActivity"
NET_TESTS_ACTIVITY="$APP_ID/dev.screenkit.net.NetTests"
NET_FIXTURE="$HERE/out/net-fixture"
# The networking suite's rows, as runtime/tests/CMakeLists.txt registers them for
# macOS, minus net-image -- it uploads the downloaded PNG to a texture and this
# binary has no drawable (runtime/tests/CMakeLists.txt) -- plus the two rows of
# the Android client's own matrix that exist nowhere else: running with no
# JavaVM captured, and the JNI references held across a teardown mid-call.
# Two of them carry a transcript per platform, because the vendor clients
# genuinely disagree about cookies and Content-Encoding
# (spec-platform-http-clients.md); the rest are one transcript for both.
NET_ROWS="net-http-get net-https net-request-body net-streaming-request net-streaming-response
          net-http-error-status net-redirect net-encoded-body net-abort-timeout net-flow-control
          net-unreachable net-cookies net-websocket net-eventsource net-shutdown-idle
          net-seam-contract
          net-no-javavm net-shutdown-mid-call"
# The video player's rows (spec-video-player.md), against the media the same
# fixture server generates with this machine's ffmpeg and serves on the http
# port. They run in the same test library, with no drawable: the Java player
# decodes into an ImageReader when there is no layout to put a plane in
# (dev/screenkit/media/VideoPlayer.java), so frames are still decoded and counted.
# The Shaka-shaped player they drive (@screenkit/shaka, compiled to shaka.hbc by
# the same build) is pushed beside the prelude.
MEDIA_ROWS="media-seam media-element media-hls media-mp4 media-dash media-live media-seek
            media-ended media-tracks media-clearkey media-licence media-licence-exchange media-bad-url
            media-unplayable
            media-interrupted media-remove media-two-players media-runtime-pause media-shutdown
            media-collected"
# Media rows that are not device rows: media-element-model drives the element
# against a scripted player on a GL drawable (a macOS DOM row), media-no-wayland
# is Linux's, and media-unavailable is its own macOS binary.
MEDIA_NOT_ON_DEVICE="media-element-model media-no-wayland media-unavailable"
# M9's `<iframe>` rows (spec-m9-iframe-instances) are deliberately **not** here,
# and the list is empty rather than missing so the reason is written down: every
# one of them needs a drawable -- an instance draws into a texture in the page's
# own share group, and the composite is read back as pixels -- and this test
# library has none. SDL's backends offer no offscreen surface either
# (core/src/gfx/GlSurfaceSdl.cpp), so a pbuffer is not a way round it. On this
# platform the instances are covered by the manual check instead: a launcher
# package embedding a game, run through `android.sh run`, screenshotted.
IFRAME_ROWS=""
# ...and `host`, which is not one of them: it runs the shipping host rather than
# the test library (see host_row). `test` runs NET_ROWS, MEDIA_ROWS, then host.
APK="$PROJECT/app/build/outputs/apk/debug/app-debug.apk"
SDKMANAGER="$SDK/cmdline-tools/latest/bin/sdkmanager"
AVDMANAGER="$SDK/cmdline-tools/latest/bin/avdmanager"
ADB="$SDK/platform-tools/adb"
EMULATOR="$SDK/emulator/emulator"
SIZE="${SIZE:-}"

cmd=${1:-}
[ $# -gt 0 ] && shift

say() { printf '>>> %s\n' "$*"; }

# This script is macOS on Apple silicon: the cmdline-tools and emulator image it
# installs are mac_arm64, the AVD is edited with BSD sed, and that is the machine
# the target was brought up on. The project itself is not macOS-only -- Gradle
# builds the APK anywhere the SDK, the NDK and CMake 3.24+ are installed.
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64) ;;
    *)
        echo "android.sh runs on macOS arm64; this is $(uname -s)/$(uname -m)." >&2
        echo "Install the Android SDK, NDK r27d and CMake 3.24+ yourself, then:" >&2
        echo "  cd runtime/android && ./gradlew assembleDebug -Pscreenkit.app=examples/pixi-hello" >&2
        exit 2
        ;;
esac

sdk() {
    if [ ! -x "$SDKMANAGER" ]; then
        say "sdk: cmdline-tools into $SDK/cmdline-tools/latest"
        tmp=$(mktemp -d)
        curl -fsSL -o "$tmp/tools.zip" "$CMDLINE_TOOLS_URL"
        got=$(shasum -a 256 "$tmp/tools.zip" | cut -d' ' -f1)
        [ "$got" = "$CMDLINE_TOOLS_SHA256" ] || { echo "checksum mismatch for cmdline-tools: $got" >&2; rm -rf "$tmp"; exit 1; }
        unzip -q "$tmp/tools.zip" -d "$tmp"
        mkdir -p "$SDK/cmdline-tools"
        rm -rf "$SDK/cmdline-tools/latest"
        mv "$tmp/cmdline-tools" "$SDK/cmdline-tools/latest"
        rm -rf "$tmp"
    fi
    say "sdk: licences"
    yes | "$SDKMANAGER" --licenses >/dev/null 2>&1 || true
    say "sdk: $NDK, $IMAGE, platform 35, build-tools 35, platform-tools, emulator"
    # sdkmanager verifies each package against the repository's own checksums.
    "$SDKMANAGER" --install "$NDK" "$IMAGE" "platforms;android-35" "build-tools;35.0.0" "platform-tools" "emulator"
}

booted() {
    [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]
}

avd() {
    if ! "$EMULATOR" -list-avds | grep -qx "$AVD"; then
        say "avd: creating $AVD from $IMAGE (tv_1080p)"
        echo no | "$AVDMANAGER" create avd -n "$AVD" -k "$IMAGE" -d tv_1080p
        # The host GPU, and a 2 GB data partition: the TV profile asks for 10 GB,
        # and the emulator refuses to start without that much free disk.
        sed -i '' -e 's/^disk.dataPartition.size=.*/disk.dataPartition.size=2G/' \
                  -e 's/^hw.gpu.enabled=.*/hw.gpu.enabled=yes/' -e 's/^hw.gpu.mode=.*/hw.gpu.mode=host/' \
            "${ANDROID_AVD_HOME:-$HOME/.android/avd}/$AVD.avd/config.ini"
    fi
    pid=
    if ! "$ADB" devices | grep -q '^emulator-'; then
        say "avd: booting $AVD"
        mkdir -p "$OUT"
        nohup "$EMULATOR" -avd "$AVD" -no-boot-anim -no-snapshot-save -gpu host >"$OUT/emulator.log" 2>&1 &
        pid=$!
    fi
    i=0
    until booted; do
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
            echo "the emulator exited:" >&2
            grep -E 'FATAL|ERROR' "$OUT/emulator.log" >&2 || tail -5 "$OUT/emulator.log" >&2
            exit 1
        fi
        i=$((i + 1))
        [ $i -le 180 ] || { echo "$AVD did not finish booting in 6 minutes; see $OUT/emulator.log" >&2; exit 1; }
        sleep 2
    done
    say "avd: $AVD is up ($("$ADB" shell getprop ro.build.version.release | tr -d '\r'), $("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r'))"
}

build() {
    app=${1:-pixi-hello}
    # The prebuilt AARs, libjsi.so and the JSI headers, verified into the cache.
    # Named one by one: the whole set would also fetch what an Android build has
    # no use for, such as ANGLE's headers.
    mkdir -p "$OUT"
    for dep in hermes sdl3 sdl3_ttf; do
        node "$ROOT/tools/prebuilts/fetch.mjs" --dep "$dep" android-arm64 android-arm32 \
            >/dev/null 2>"$OUT/fetch.log" || { cat "$OUT/fetch.log" >&2; exit 1; }
    done
    # CMake and Ninja from PATH: runtime/CMakeLists.txt needs CMake 3.24+, newer
    # than the SDK's own CMake packages Gradle would otherwise download.
    cmake_bin=$(command -v cmake) || { echo "cmake is not on PATH" >&2; exit 1; }
    command -v ninja >/dev/null || { echo "ninja is not on PATH" >&2; exit 1; }
    printf 'sdk.dir=%s\ncmake.dir=%s\n' "$SDK" "$(dirname "$(dirname "$cmake_bin")")" >"$PROJECT/local.properties"
    say "build: debug APK with examples/$app bundled${GRADLE_EXTRA:+ ($GRADLE_EXTRA)}"
    # shellcheck disable=SC2086 -- GRADLE_EXTRA is a list of gradle flags.
    (cd "$PROJECT" && ./gradlew --quiet assembleDebug -Pscreenkit.app="examples/$app" ${GRADLE_EXTRA:-})
    unzip -l "$APK" | awk '/lib\// {print "    " $4 "  " $1}'
    say "apk: $APK"
}

install() {
    [ -f "$APK" ] || { echo "no APK: sh tools/android/android.sh build" >&2; exit 1; }
    say "install: $APK"
    "$ADB" install -r "$APK" >/dev/null
}

push() {
    app=${1:?usage: android.sh push <app>}
    pkg="$ROOT/examples/$app/app.skpkg"
    [ -f "$pkg/manifest.json" ] || (cd "$ROOT/examples/$app" && npm run --silent build:screenkit >/dev/null)
    say "push: $app.skpkg ($(du -sh "$pkg" | cut -f1)) to $APP_ID files/apps/"
    # adb can write /data/local/tmp but not the app's own storage; run-as, as the
    # app, copies it in -- which is why this needs a debug build.
    "$ADB" shell rm -rf "/data/local/tmp/screenkit/$app.skpkg"
    "$ADB" shell mkdir -p /data/local/tmp/screenkit
    "$ADB" push "$pkg" "/data/local/tmp/screenkit/$app.skpkg" >/dev/null
    "$ADB" shell run-as "$APP_ID" sh -c "'rm -rf files/apps/$app.skpkg && mkdir -p files/apps && cp -R /data/local/tmp/screenkit/$app.skpkg files/apps/'"
    "$ADB" shell rm -rf "/data/local/tmp/screenkit/$app.skpkg"
}

# am start extras for <app>: a pushed package by name, or the APK's own.
extras() {
    app=$1
    [ "$app" = bundled ] || printf -- '--es package %s ' "$app"
    [ -z "$SIZE" ] || printf -- '--es size %s ' "$SIZE"
}

log() {
    "$ADB" logcat -d -v brief ScreenKit:V SDL:V SDL/APP:V AndroidRuntime:E DEBUG:V libc:F '*:S' | grep -v 'frame time' || true
}

run() {
    app=${1:?usage: android.sh run <app|bundled> [seconds]}
    secs=${2:-15}
    [ "$app" = bundled ] || [ ! -d "$ROOT/examples/$app" ] || push "$app"
    say "run: $app for ${secs}s"
    "$ADB" logcat -c
    # -S: stop a running instance first, so the extras are this launch's.
    # shellcheck disable=SC2046
    "$ADB" shell am start -S -W -n "$ACTIVITY" $(extras "$app") ${CAPTURE_EXTRAS:-} >/dev/null
    sleep "$secs"
    log
}

# \x89PNG\r\n\x1a\n, the 8-byte signature every PNG starts with.
is_png() {
    [ -s "$1" ] && [ "$(od -An -N4 -tx1 "$1" | tr -d ' \n')" = "89504e47" ]
}

shot() {
    app=${1:?usage: android.sh shot <app|bundled> [seconds]}
    secs=${2:-15}
    delay=$(( (secs - 5) * 1000 ))
    [ $delay -gt 0 ] || delay=1000
    mkdir -p "$OUT"
    "$ADB" shell run-as "$APP_ID" rm -f "files/captures/$app.png" 2>/dev/null || true
    CAPTURE_EXTRAS="--es capture $app.png --ei captureDelayMs $delay" run "$app" "$secs"
    # The frame the app drew, read back from GL; and what the display shows.
    "$ADB" exec-out run-as "$APP_ID" cat "files/captures/$app.png" >"$OUT/$app.png"
    "$ADB" exec-out screencap -p >"$OUT/$app-screen.png"
    # exec-out writes run-as's complaint into the file when there is no capture,
    # so the PNG signature decides, not the size.
    if is_png "$OUT/$app.png"; then
        say "frame: $OUT/$app.png"
    else
        echo "no frame captured: $(head -c 200 "$OUT/$app.png")" >&2
        rm -f "$OUT/$app.png"
    fi
    if is_png "$OUT/$app-screen.png"; then
        say "screen: $OUT/$app-screen.png"
    else
        echo "no screenshot: $(head -c 200 "$OUT/$app-screen.png")" >&2
        rm -f "$OUT/$app-screen.png"
    fi
}

key() {
    for k in "$@"; do
        "$ADB" shell input keyevent "$k"
        sleep 1
    done
}

logs() {
    # ScreenKitNetTests too: the device rows log under their own tag, and without
    # it `android.sh logs` shows nothing at all during a test run.
    "$ADB" logcat -v time ScreenKit:V ScreenKitNetTests:V SDL:V stderr:V AndroidRuntime:E DEBUG:V '*:S'
}

# ---- the runtime's networking rows, on the device ------------------------------
#
# The same rows, the same fixture server and the same transcripts as the macOS
# suite (runtime/tests/CMakeLists.txt): what changes is where they run. The node
# fixture stays on this machine, on loopback, and the device reaches it through
# `adb reverse` on the same port numbers -- so the certificates' 127.0.0.1 name
# still matches and no row touches a network beyond the two loopbacks.
#
# A row runs inside the app, not from /data/local/tmp: it needs a JavaVM whose
# class loader can see dev.screenkit.net.HttpClient, the INTERNET permission and
# the app's own storage for the cookie jar (runtime/tests/CMakeLists.txt).

json_port() {
    sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p" "$1" | head -1
}

# Only the forwards this run created: `adb reverse --remove-all` would tear down
# whatever else the developer had set up on the same device.
REVERSED=
fixture_stop() {
    sh "$ROOT/runtime/tests/net/stop-server.sh" "$NET_FIXTURE" >/dev/null 2>&1 || true
    for p in $REVERSED; do
        "$ADB" reverse --remove "tcp:$p" >/dev/null 2>&1 || true
    done
    REVERSED=
}

# Every `net-*` case CTest runs must be in NET_ROWS, or a row added later would
# run on Apple only while this script still reported that every row passed.
# net-image is the one deliberate exception (no drawable here); the rest of the
# names below are CTest's fixture entries, not rows.
check_rows() {
    missing=
    # A case name is a line of its own in SCREENKIT_TEST_CASES /
    # SCREENKIT_NET_TEST_CASES, optionally closing the list. Anchoring on that
    # keeps target names such as `screenkit-net-tests` out of the comparison.
    for name in $(grep -oE '^[[:space:]]*net-[a-z0-9-]+\)?[[:space:]]*$' \
                       "$ROOT/runtime/tests/CMakeLists.txt" | tr -d ' \t)' | sort -u); do
        case "$name" in
            net-image) continue ;;
        esac
        printf '%s\n' $NET_ROWS | grep -qx "$name" || missing="$missing $name"
    done
    [ -z "$missing" ] || {
        echo "these CTest net rows are not in NET_ROWS and would never run on a device:$missing" >&2
        exit 1
    }
    # The same for the media rows: every `media-*` case CTest runs is one here.
    for name in $(grep -oE '^[[:space:]]*media-[a-z0-9-]+\)?[[:space:]]*$' \
                       "$ROOT/runtime/tests/CMakeLists.txt" | tr -d ' \t)' | sort -u); do
        printf '%s\n' $MEDIA_NOT_ON_DEVICE | grep -qx "$name" && continue
        printf '%s\n' $MEDIA_ROWS | grep -qx "$name" || missing="$missing $name"
    done
    [ -z "$missing" ] || {
        echo "these CTest media rows are not in MEDIA_ROWS and would never run on a device:$missing" >&2
        exit 1
    }
}

# One row: relaunch the activity, then wait for the line it logs when it is done.
#
# The wait is bound to *this* launch by a nonce the activity echoes back. Polling
# for a bare `rc=` was not: `logcat -c` can fail (it does on a busy ring buffer),
# and the previous run's line would then be read as this one's result -- a PASS
# for a row that never ran.
net_row() {
    row=$1
    nonce=$2
    "$ADB" logcat -c >/dev/null 2>&1 || true
    "$ADB" shell am start -S -n "$NET_TESTS_ACTIVITY" --es case "$row" --es nonce "$nonce" >/dev/null
    i=0
    while [ $i -lt 180 ]; do
        line=$("$ADB" logcat -d -s ScreenKitNetTests:I 2>/dev/null \
               | grep "net-row $row nonce=$nonce rc=" | tail -1 || true)
        if [ -n "$line" ]; then
            printf '%s' "${line##*rc=}" | tr -d '\r'
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    printf 'timeout'
}

# The one row that runs on the shipping host: ScreenKitActivity, libscreenkit.so
# and its JNI_OnLoad, with a package that fetches the fixture and says what it
# got. Nothing else here loads that library, so without this the host's own
# wiring could be deleted and every row would still pass.
host_row() {
    hermesc=$("$NODE" "$ROOT/tools/prebuilts/fetch.mjs" --hermesc 2>/dev/null | tail -1)
    [ -n "$hermesc" ] || { echo "no hermesc: node tools/prebuilts/fetch.mjs --hermesc" >&2; return 1; }
    pkg="$OUT/net-host.skpkg"
    "$NODE" "$HERE/net-host-app.mjs" "$hermesc" "$pkg" "http://127.0.0.1:$1" >/dev/null

    "$ADB" shell rm -rf /data/local/tmp/screenkit/net-host.skpkg
    "$ADB" shell mkdir -p /data/local/tmp/screenkit
    "$ADB" push "$pkg" /data/local/tmp/screenkit/net-host.skpkg >/dev/null
    "$ADB" shell run-as "$APP_ID" sh -c "'rm -rf files/apps/net-host.skpkg && mkdir -p files/apps && cp -R /data/local/tmp/screenkit/net-host.skpkg files/apps/'"
    "$ADB" shell rm -rf /data/local/tmp/screenkit/net-host.skpkg

    "$ADB" logcat -c >/dev/null 2>&1 || true
    "$ADB" shell am start -S -n "$ACTIVITY" --es package net-host >/dev/null
    i=0
    while [ $i -lt 90 ]; do
        line=$("$ADB" logcat -d -s ScreenKit:I 2>/dev/null | grep 'host-net ' | tail -1 || true)
        if [ -n "$line" ]; then
            printf '%s' "${line#*host-net }" | tr -d '\r'
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    printf 'timeout'
}

test_rows() {
    [ -n "$("$ADB" devices | sed -n '2p')" ] || { echo "no device: sh tools/android/android.sh avd" >&2; exit 1; }
    NODE=$(command -v node) || { echo "node is not on PATH" >&2; exit 1; }
    node_bin=$NODE
    check_rows

    # MINIFY=1: the same rows against an R8-processed APK, which is what checks
    # runtime/android/app/proguard-rules.pro.
    GRADLE_EXTRA="-Pscreenkit.netTests${MINIFY:+ -Pscreenkit.minify}" build
    unzip -l "$APK" | grep -q 'libscreenkit-net-tests.so' \
        || { echo "the APK carries no libscreenkit-net-tests.so" >&2; exit 1; }
    install

    say "test: starting the fixture server"
    mkdir -p "$OUT"
    trap fixture_stop EXIT INT TERM
    sh "$ROOT/runtime/tests/net/start-server.sh" "$node_bin" \
        "$ROOT/runtime/tests/net/server.mjs" "$NET_FIXTURE" >/dev/null

    # Every listening port, on the same number on both sides. closedPort is
    # deliberately not published: net-unreachable needs it refused.
    ports=""
    for key in http https httpsUntrusted httpsExpired httpsWrongHost; do
        port=$(json_port "$NET_FIXTURE/servers.json" "$key")
        [ -n "$port" ] || { echo "the fixture published no $key port" >&2; exit 1; }
        "$ADB" reverse "tcp:$port" "tcp:$port" >/dev/null
        REVERSED="$REVERSED $port"
        ports="$ports $port"
    done
    http_port=$(json_port "$NET_FIXTURE/servers.json" http)
    say "test: adb reverse$ports"

    # servers.json, the test CA and the prelude, into the app's own storage --
    # adb can write /data/local/tmp, and run-as copies it in as the app.
    shim="$PROJECT/app/build/generated/screenkit/assets/screenkit/dom-shim.hbc"
    [ -f "$shim" ] || { echo "no prelude at $shim" >&2; exit 1; }
    # @screenkit/shaka as the test library's own build compiled it
    # (runtime/CMakeLists.txt, screenkit-shaka-script), newest first.
    shaka=$(find "$PROJECT/app/.cxx" -name shaka.hbc -path '*arm64-v8a*' -exec ls -t {} + 2>/dev/null | head -1)
    [ -n "$shaka" ] && [ -f "$shaka" ] || { echo "no shaka.hbc under $PROJECT/app/.cxx" >&2; exit 1; }
    "$ADB" shell rm -rf /data/local/tmp/screenkit/net-fixture
    "$ADB" shell mkdir -p /data/local/tmp/screenkit/net-fixture
    "$ADB" push "$NET_FIXTURE/servers.json" /data/local/tmp/screenkit/net-fixture/ >/dev/null
    "$ADB" push "$NET_FIXTURE/ca.der" /data/local/tmp/screenkit/net-fixture/ >/dev/null
    "$ADB" push "$shim" /data/local/tmp/screenkit/net-fixture/dom-shim.hbc >/dev/null
    "$ADB" push "$shaka" /data/local/tmp/screenkit/net-fixture/shaka.hbc >/dev/null
    "$ADB" shell run-as "$APP_ID" sh -c "'rm -rf files/net-fixture && mkdir -p files && cp -R /data/local/tmp/screenkit/net-fixture files/'"
    # A CHECK writes to stderr, which is /dev/null in an app unless this is set:
    # without it a failing row says only that it failed.
    "$ADB" shell setprop log.redirect-stdio true >/dev/null 2>&1 || true

    rows=${*:-$NET_ROWS $MEDIA_ROWS host}
    failed=0
    launch=0
    for row in $rows; do
        launch=$((launch + 1))
        if [ "$row" = host ]; then
            # The shipping host, not the test library: see host_row.
            got=$(host_row "$http_port")
            case "$got" in
                ok\ 200\ 42) say "PASS host (libscreenkit.so: $got)" ;;
                *) echo "FAIL host -- fetch through the real host said: $got" >&2
                   failed=$((failed + 1))
                   "$ADB" logcat -d -v brief ScreenKit:V AndroidRuntime:E DEBUG:V '*:S' | tail -40 >&2 ;;
            esac
            continue
        fi
        rc=$(net_row "$row" "$$-$launch-$(date +%s)")
        case "$rc" in
            0) say "PASS $row" ;;
            # The matrix is explicit: a row that skipped for "no HTTP client"
            # is a failure here, not a pass.
            77) echo "SKIP $row -- counted as a failure" >&2; failed=$((failed + 1)) ;;
            *)  echo "FAIL $row (rc=$rc)" >&2; failed=$((failed + 1))
                "$ADB" logcat -d -v brief ScreenKitNetTests:V ScreenKit:V stderr:V AndroidRuntime:E DEBUG:V '*:S' | tail -60 >&2 ;;
        esac
    done
    if [ "$failed" -gt 0 ]; then
        echo "$failed row(s) failed" >&2
        exit 1
    fi
    say "test: every row passed"
}

case "$cmd" in
    sdk) sdk ;;
    avd) avd ;;
    build) build "$@" ;;
    install) install ;;
    push) push "$@" ;;
    run) run "$@" ;;
    shot) shot "$@" ;;
    key) key "$@" ;;
    logs) logs ;;
    test) test_rows "$@" ;;
    *)
        sed -n '2,20p' "$0"
        exit 2
        ;;
esac
