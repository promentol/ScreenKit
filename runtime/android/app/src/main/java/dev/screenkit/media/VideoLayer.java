// Copyright (c) ScreenKit contributors. MIT.
package dev.screenkit.media;

import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

import java.lang.ref.WeakReference;

/**
 * Where video planes go on Android: beneath SDL's surface, in SDL's own layout.
 *
 * <p>SDL's {@code SurfaceView} is z-ordered as a media overlay
 * ({@link #attachAppSurface}, from {@code ScreenKitActivity.createSDLSurface}),
 * so every video {@code SurfaceView} -- an ordinary one, at the media layer --
 * sits beneath it, and both sit beneath the window, which is transparent where
 * they are. The app's frame shows the video wherever it cleared to transparent.
 *
 * <p>SDL's surface carries alpha (an RGBA EGL config, GlSurfaceSdl.cpp) but is
 * created <em>opaque</em> to the compositor -- the default format -- which is
 * exactly what it was before video existed: a page that never makes a
 * {@code <video>} is composited as it always was. The first video player a page
 * creates switches the surface to {@code TRANSLUCENT}, once and for good. A
 * SurfaceView's opacity cannot be changed in place: its buffers live in a BLAST
 * child layer, created with the format's OPAQUE flag, that no public API
 * reaches -- {@code SurfaceControl.Transaction.setOpaque} on the SurfaceView's
 * own control does not touch it (measured: SurfaceFlinger kept the child
 * opaque and the video hidden). So the switch is a format change, which Android
 * performs by recreating the surface: the host's existing
 * leave-and-return-to-screen path (runtime/android/jni/HostMain.cpp) pauses the
 * runtime, takes the new drawable and presents the last frame again. It
 * happens when the player is made, before any plane shows.
 */
public final class VideoLayer {
    private static final Handler ui = new Handler(Looper.getMainLooper());
    private static WeakReference<SurfaceView> appSurface = new WeakReference<>(null);
    private static boolean translucent;

    private VideoLayer() {}

    /** SDL's surface, before it is attached to the window. From ScreenKitActivity. UI thread. */
    public static void attachAppSurface(SurfaceView surface) {
        appSurface = new WeakReference<>(surface);
        surface.setZOrderMediaOverlay(true);
        if (translucent) surface.getHolder().setFormat(PixelFormat.TRANSLUCENT);
    }

    /** A video player exists: from now on SDL's surface lets planes beneath show. Any thread. */
    static void videoCreated() {
        ui.post(() -> {
            if (translucent) return;
            SurfaceView surface = appSurface.get();
            if (surface == null) return;  // no host surface: the test harness
            translucent = true;
            surface.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        });
    }

    /** The layout planes go into, or null when there is none (the test harness). UI thread. */
    static ViewGroup layout() {
        View content = SDLActivity.getContentView();
        return content instanceof ViewGroup ? (ViewGroup) content : null;
    }
}
