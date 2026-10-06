// Copyright (c) ScreenKit contributors. MIT.
package dev.screenkit.net;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import com.facebook.soloader.nativeloader.NativeLoader;
import com.facebook.soloader.nativeloader.SystemDelegate;

import org.libsdl.app.SDL;

import java.io.File;

/**
 * One row of the runtime's networking suite, run inside the app process.
 *
 * <p>Debug builds only, and never shipped: this class lives in {@code src/debug}
 * and the library it loads is built only when Gradle is asked for it
 * ({@code -Pscreenkit.netTests}).
 *
 * <p>The rows have to run here rather than from a binary pushed to
 * {@code /data/local/tmp}, because three of the things a row needs exist only in
 * an app process: a JavaVM whose class loader can see {@link HttpClient}, the
 * INTERNET permission, and the app's own storage for the cookie jar. The fixture
 * server stays on the development machine and is reached over {@code adb
 * reverse}, so no row touches a network beyond the emulator's loopback.
 *
 * <pre>
 *   am start -S -n dev.screenkit.host/dev.screenkit.net.NetTests \
 *       --es case net-http-get --es nonce &lt;anything&gt;
 * </pre>
 *
 * <p>The result is one logcat line under tag {@code ScreenKitNetTests}:
 * {@code net-row <case> nonce=<nonce> rc=<0 ok | 77 skipped | anything else failed>}.
 * The nonce is echoed so a reader can bind the line to the launch it made.
 * tools/android/android.sh test drives the loop and treats a skip as a failure,
 * as the spec's matrix says.
 */
public final class NetTests extends Activity {
    private static final String TAG = "ScreenKitNetTests";
    /** Where android.sh pushes servers.json, ca.der and the prelude. */
    private static final String FIXTURE_DIR = "net-fixture";
    private static final String DOM_SHIM = "dom-shim.hbc";
    /** @screenkit/shaka for the media rows, pushed beside the prelude. */
    private static final String SHAKA = "shaka.hbc";
    /** The row that runs the client with no JavaVM captured. */
    private static final String NO_JAVAVM_ROW = "net-no-javavm";

    private static boolean loaded;
    private String mNonce = "";

    /** Returns CTest's own codes: 0 ran, 77 skipped, 1 failed, 64 unknown row. */
    private static native int nativeRun(String name, String fixtures, String netFixture,
                                        String domShim);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0) {
            Log.e(TAG, "the net rows run in debuggable builds only");
            done("none", 64);
            return;
        }
        Intent intent = getIntent();
        final String row = intent != null ? intent.getStringExtra("case") : null;
        // Echoed back in the result line so the script can tell this launch's
        // answer from the last one's: `logcat -c` is not guaranteed to have taken.
        String given = intent != null ? intent.getStringExtra("nonce") : null;
        mNonce = given == null ? "" : given;
        if (row == null || row.isEmpty()) {
            Log.e(TAG, "no row: pass --es case <name>");
            done("none", 64);
            return;
        }
        // Every row starts from an empty platform cookie store. The store is the
        // device's WebView one and outlives this process once Chromium flushes it
        // to disk, which it may do whenever it likes -- so without this,
        // `net-cookies` would one day start seeing the previous run's cookies in
        // a header it asserts exactly. Cleared from here because the callback
        // needs a Looper, and the row is started from the callback so nothing
        // it does can overtake the clearing.
        android.webkit.CookieManager cookies = null;
        try {
            cookies = android.webkit.CookieManager.getInstance();
        } catch (Throwable noWebView) {
            Log.w(TAG, "no android.webkit.CookieManager: the client keeps cookies in memory");
        }
        if (cookies == null) {
            start(row);
            return;
        }
        final android.webkit.CookieManager store = cookies;
        store.removeAllCookies(new android.webkit.ValueCallback<Boolean>() {
            @Override
            public void onReceiveValue(Boolean removed) {
                store.flush();
                start(row);
            }
        });
    }

    private void start(final String row) {
        // Off the UI thread: a row blocks for as long as its transcript takes,
        // and the runtime it creates starts its own JS thread.
        new Thread(new Runnable() {
            @Override
            public void run() {
                int code;
                try {
                    code = runRow(row);
                } catch (Throwable e) {
                    Log.e(TAG, "the row threw", e);
                    code = 1;
                }
                done(row, code);
            }
        }, "screenkit-net-tests").start();
    }

    private int runRow(String row) {
        // The one row that needs the client never to have been given a JavaVM.
        // It has to be set before the library is loaded, because JNI_OnLoad is
        // where the VM would be captured; after that there is no way back.
        if (NO_JAVAVM_ROW.equals(row)) {
            try {
                Os.setenv("SCREENKIT_NET_TESTS_NO_BACKEND", "1", true);
            } catch (ErrnoException e) {
                Log.e(TAG, "cannot withhold the JavaVM: " + e);
                return 1;
            }
        }
        load(this);
        File fixture = new File(getFilesDir(), FIXTURE_DIR);
        String shim = new File(fixture, DOM_SHIM).getPath();
        // The media rows read it from here (runtime/tests/RuntimeTests.cpp, installShaka).
        try {
            Os.setenv("SCREENKIT_SHAKA_HBC", new File(fixture, SHAKA).getPath(), true);
        } catch (ErrnoException e) {
            Log.e(TAG, "cannot name shaka.hbc for the media rows: " + e);
        }
        Log.i(TAG, "net-row " + row + " starting, fixture " + fixture);
        return nativeRun(row, fixture.getPath(), fixture.getPath(), shim);
    }

    /**
     * The same libraries ScreenKitActivity loads, with the test library in place
     * of the host's: loading both would leave two copies of the client racing to
     * register {@link HttpClient}'s callbacks.
     *
     * <p>Then the two steps {@code SDLActivity.onCreate} would do and this
     * activity is not: {@code SDL.setupJNI()} caches SDL's own Java classes and
     * method ids on the native side, and {@code SDL.setContext} gives it an
     * Activity. The runtime asks SDL for the events subsystem and its timers
     * (core/src/hermes/HermesHost.cpp), and SDL's Android layer reaches back into
     * Java for the environment before it will start -- with nothing cached that
     * is a call on a null jclass, which ART turns into an abort.
     */
    private static synchronized void load(Activity activity) {
        if (loaded) return;
        NativeLoader.initIfUninitialized(new SystemDelegate());
        for (String library : new String[] {"SDL3", "SDL3_ttf", "fbjni", "hermesvm",
                                            "screenkit-net-tests"}) {
            System.loadLibrary(library);
        }
        SDL.setupJNI();
        SDL.initialize();
        SDL.setContext(activity);
        loaded = true;
    }

    /**
     * The line android.sh waits for, and then the process: the runtime's state is
     * process-wide, so the next row starts from nothing.
     */
    private void done(String row, int code) {
        Log.i(TAG, "net-row " + row + " nonce=" + mNonce + " rc=" + code);
        finish();
        System.exit(0);
    }
}
