// Copyright (c) ScreenKit contributors. MIT.
package dev.screenkit.host;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import android.view.SurfaceHolder;

import com.facebook.soloader.nativeloader.NativeLoader;

import dev.screenkit.media.VideoLayer;
import com.facebook.soloader.nativeloader.SystemDelegate;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * The ScreenKit shell on Android TV and Fire TV: SDL's activity, running
 * libscreenkit.so's SDL_main (runtime/android/jni/HostMain.cpp). Everything the
 * app is lives in the runtime; this class decides only what to run and hands
 * that over as SDL_main's arguments.
 *
 * <p>The activity is exported -- the launcher starts it -- so the extras below
 * are honoured only in a debuggable build, and name files inside the app's own
 * storage. In a release build any other app could otherwise ask ScreenKit to run
 * JS of its choosing, with ScreenKit's storage and permissions.
 *
 * <p>The native host opens files by path, so what the APK carries in
 * {@code assets/screenkit/} -- the prelude {@code dom-shim.hbc} and the bundled
 * {@code app.skpkg} -- is copied out to internal storage first, whenever the
 * installed APK differs from the one they were copied from. Intent extras, for
 * development over adb:
 *
 * <pre>
 *   am start -S -n dev.screenkit.host/.ScreenKitActivity \
 *       --es package phaser-hello          a package named in files/apps/
 *       --es size 640x480                  draw at that size, scaled to the screen
 *       --es capture shot.png              save one frame to files/captures/shot.png
 *       --ei captureDelayMs 5000           ...this long after the first frame
 * </pre>
 */
public class ScreenKitActivity extends SDLActivity {
    private static final String TAG = "ScreenKit";

    private static final String ASSETS = "screenkit";
    private static final String BUNDLED_PACKAGE = "app.skpkg";
    private static final String DOM_SHIM = "dom-shim.hbc";

    private String[] mArguments = new String[0];

    @Override
    protected String[] getLibraries() {
        // Dependencies first. fbjni's JNI_OnLoad is what gives hermesvm's Intl its
        // JavaVM; libjsi.so and libc++_shared.so load as hermesvm's dependencies.
        return new String[] {"SDL3", "SDL3_ttf", "fbjni", "hermesvm", "screenkit"};
    }

    @Override
    public void loadLibraries() {
        // fbjni's Java classes load their library through SoLoader's NativeLoader,
        // which throws unless told how; plain System.loadLibrary is right here.
        NativeLoader.initIfUninitialized(new SystemDelegate());
        super.loadLibraries();
    }

    @Override
    protected String[] getArguments() {
        return mArguments;
    }

    /**
     * SDL's own thread, before SDL_main: the copy out of the APK happens here
     * rather than on the UI thread, where a large package would stall the
     * activity's start.
     */
    @Override
    protected void main() {
        mArguments = prepareLaunch(getIntent());
        super.main();
    }

    // ---- leaving and returning to the screen ------------------------------------
    //
    // The runtime lets go of the window's drawable here, on the UI thread, before
    // SDL is told of the pause and releases the surface: SDL's own background
    // event arrives inside a lock where the native host cannot wait
    // (runtime/android/jni/HostMain.cpp, Lifecycle). Both calls are idempotent.

    private static native void nativeLeaveScreen();
    private static native void nativeReturnToScreen();

    @Override
    protected void onPause() {
        if (!mBrokenLibraries) nativeLeaveScreen();
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mBrokenLibraries) nativeReturnToScreen();
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface surface = new SDLSurface(context) {
            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                // Also here: a surface can go without the activity pausing first.
                if (!mBrokenLibraries) nativeLeaveScreen();
                super.surfaceDestroyed(holder);
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                super.surfaceChanged(holder, format, width, height);
                // SDL recreates the window's EGL surface inside that call, on this
                // thread, so this is the first moment there is a drawable to bind.
                // onResume can arrive before it, with nothing to bind yet.
                if (!mBrokenLibraries) nativeReturnToScreen();
            }
        };
        // <video> planes go beneath this surface: it is z-ordered as a media
        // overlay, above the video SurfaceViews, before it joins the window, and
        // turns translucent when the page makes its first video (VideoLayer).
        VideoLayer.attachAppSurface(surface);
        return surface;
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Bundle extras = intent.getExtras();
        if (extras != null && !extras.isEmpty()) {
            Log.w(TAG, "already running, so this launch's extras are ignored -- `am start -S` relaunches");
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // SDL's native state, and the runtime's, is process-wide and not made to
        // start twice: an activity that has finished takes its process with it,
        // so the next launch starts clean.
        if (isFinishing()) {
            System.exit(0);
        }
    }

    @SuppressWarnings("deprecation") // Bundle.get: the extras are strings or ints, whichever am start sent
    private String[] prepareLaunch(Intent intent) {
        File files = getFilesDir();
        File bundle = new File(files, "bundle");
        try {
            extractBundle(files, bundle);
        } catch (IOException e) {
            // The intent's package still runs, if it named one: what is missing is
            // the APK's own copy -- the prelude, and the bundled package. SDL_main
            // says which in logcat and ends if it cannot go on.
            Log.e(TAG, "could not copy the APK's assets out", e);
        }
        List<String> args = new ArrayList<>();

        Bundle extras = launchExtras(intent);
        File pkg = null;
        String named = extras != null ? extras.getString("package") : null;
        if (named != null && !named.isEmpty()) {
            // Confined to files/apps/: the activity is exported, and a path from
            // outside would pick what runs inside this app's sandbox.
            String name = named.endsWith(".skpkg") ? named : named + ".skpkg";
            pkg = confined(new File(files, "apps"), name, "package");
            if (pkg != null) Log.i(TAG, "package from the intent: " + pkg);
        }
        if (pkg == null && new File(bundle, BUNDLED_PACKAGE).isDirectory()) {
            pkg = new File(bundle, BUNDLED_PACKAGE);
        }
        // A package that is not there is the native gate's to refuse, in logcat.
        if (pkg != null) {
            args.add("--package");
            args.add(pkg.getPath());
        }
        args.add("--dom-shim");
        args.add(new File(bundle, DOM_SHIM).getPath());

        if (extras != null) {
            Object size = extras.get("size");
            if (size != null) {
                args.add("--size");
                args.add(size.toString());
            }
            Object capture = extras.get("capture");
            File file = capture == null ? null
                    : confined(new File(files, "captures"), capture.toString(), "capture");
            if (file != null && (file.getParentFile().isDirectory() || file.getParentFile().mkdirs())) {
                args.add("--capture");
                args.add(file.getPath());
                Object delay = extras.get("captureDelayMs");
                if (delay != null) {
                    args.add("--capture-delay-ms");
                    args.add(delay.toString());
                }
            } else if (file != null) {
                Log.w(TAG, "cannot create " + file.getParentFile() + ": no frame will be captured");
            }
        }
        return args.toArray(new String[0]);
    }

    /**
     * The launch extras, in a debuggable build. This activity is exported, so in
     * a release build they are somebody else's choice of what ScreenKit runs.
     */
    private Bundle launchExtras(Intent intent) {
        Bundle extras = intent != null ? intent.getExtras() : null;
        if (extras == null || extras.isEmpty()) return null;
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) return extras;
        Log.w(TAG, "launch extras are for debuggable builds only, ignoring: " + extras.keySet());
        return null;
    }

    /**
     * A plain name inside one of the app's own directories. A name with a path in
     * it -- a separator, `..`, or an absolute path -- is refused rather than
     * resolved, which is the whole check: everything else is a name.
     */
    private static File confined(File directory, String name, String what) {
        if (name.isEmpty() || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0
                || name.equals(".") || name.equals("..")) {
            Log.w(TAG, "the " + what + " extra takes a name in " + directory + ", not a path: " + name);
            return null;
        }
        return new File(directory, name);
    }

    /**
     * files/bundle/: the APK's assets/screenkit/, copied out once per installed
     * APK. The copy is built beside it and renamed into place with its version
     * stamp already inside, so an interrupted copy is never taken for a finished
     * one, and a copy from an older APK never runs. Every file is synced before
     * the rename, and the directory after it: a TV loses power without warning,
     * and an unsynced copy came back from an emulator kill as zero-length files
     * under a valid stamp's name.
     */
    private void extractBundle(File files, File bundle) throws IOException {
        String version = installedVersion();
        File stamp = new File(bundle, ".apk-version");
        if (stamp.isFile() && version.equals(readText(stamp))) {
            return;
        }

        long started = System.currentTimeMillis();
        File staging = new File(files, "bundle.staging");
        deleteRecursively(staging);
        // Before the copy, not after it: a device with a nearly full disk cannot
        // hold the old copy, the new one and the APK's compressed assets at once,
        // and the old one is already known to be the wrong version.
        deleteRecursively(bundle);
        copyAssets(getAssets(), ASSETS, staging);
        writeText(new File(staging, ".apk-version"), version);
        if (!staging.renameTo(bundle)) {
            throw new IOException("cannot move " + staging + " to " + bundle);
        }
        syncDirectory(files);
        Log.i(TAG, "extracted the APK's assets for " + version + " in "
                + (System.currentTimeMillis() - started) + " ms");
    }

    /**
     * Changes with every install, not only with versionCode: a development build
     * reinstalled with a different package keeps its versionCode.
     */
    @SuppressWarnings("deprecation") // PackageInfo.versionCode, below API 28
    private String installedVersion() throws IOException {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            long code = Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
            return code + "/" + info.lastUpdateTime;
        } catch (Exception e) {
            throw new IOException("cannot read this package's version: " + e, e);
        }
    }

    /** An asset path is a file if it opens, else a directory (AssetManager has no stat). */
    private static void copyAssets(AssetManager assets, String path, File target) throws IOException {
        InputStream in;
        try {
            in = assets.open(path);
        } catch (IOException notAFile) {
            in = null;
        }
        if (in != null) {
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                throw new IOException("cannot create " + parent);
            }
            try (InputStream from = in; FileOutputStream to = new FileOutputStream(target)) {
                byte[] buffer = new byte[1 << 16];
                for (int n; (n = from.read(buffer)) > 0; ) {
                    to.write(buffer, 0, n);
                }
                to.getFD().sync();
            }
            return;
        }
        String[] children = assets.list(path);
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("cannot create " + target);
        }
        if (children == null) return;
        for (String child : children) {
            copyAssets(assets, path + "/" + child, new File(target, child));
        }
    }

    private static void deleteRecursively(File file) throws IOException {
        if (!file.exists()) return;
        File[] children = file.isDirectory() ? file.listFiles() : null;
        if (children != null) {
            for (File child : children) deleteRecursively(child);
        }
        if (!file.delete()) {
            throw new IOException("cannot delete " + file);
        }
    }

    // java.io rather than java.nio.file, which Android has only from API 26.
    private static String readText(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[256];
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8).trim();
        }
    }

    private static void writeText(File file, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
    }

    /** fsync a directory, so a rename inside it survives a power cut. */
    private static void syncDirectory(File dir) throws IOException {
        try {
            FileDescriptor fd = Os.open(dir.getPath(), OsConstants.O_RDONLY, 0);
            try {
                Os.fsync(fd);
            } finally {
                Os.close(fd);
            }
        } catch (ErrnoException e) {
            throw new IOException("cannot sync " + dir + ": " + e, e);
        }
    }
}
