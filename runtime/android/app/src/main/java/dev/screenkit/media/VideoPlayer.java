// Copyright (c) ScreenKit contributors. MIT.
package dev.screenkit.media;

import android.content.Context;
import android.graphics.ImageFormat;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaDrm;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Base64;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.RelativeLayout;

import androidx.annotation.OptIn;
import androidx.media3.common.C;
import androidx.media3.common.Format;
import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.PlaybackParameters;
import androidx.media3.common.Player;
import androidx.media3.common.Timeline;
import androidx.media3.common.TrackSelectionOverride;
import androidx.media3.common.TrackSelectionParameters;
import androidx.media3.common.Tracks;
import androidx.media3.common.VideoSize;
import androidx.media3.common.text.Cue;
import androidx.media3.common.text.CueGroup;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.common.util.Util;
import androidx.media3.datasource.DataSource;
import androidx.media3.datasource.DefaultDataSource;
import androidx.media3.datasource.DefaultHttpDataSource;
import androidx.media3.datasource.HttpDataSource;
import androidx.media3.exoplayer.DecoderCounters;
import androidx.media3.exoplayer.DefaultRenderersFactory;
import androidx.media3.exoplayer.DecoderReuseEvaluation;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.analytics.AnalyticsListener;
import androidx.media3.exoplayer.dash.DashMediaSource;
import androidx.media3.exoplayer.drm.DefaultDrmSessionManager;
import androidx.media3.exoplayer.drm.DrmSessionManager;
import androidx.media3.exoplayer.drm.ExoMediaDrm;
import androidx.media3.exoplayer.drm.FrameworkMediaDrm;
import androidx.media3.exoplayer.drm.LocalMediaDrmCallback;
import androidx.media3.exoplayer.drm.MediaDrmCallback;
import androidx.media3.exoplayer.drm.MediaDrmCallbackException;
import androidx.media3.exoplayer.hls.HlsMediaSource;
import androidx.media3.exoplayer.mediacodec.MediaCodecSelector;
import androidx.media3.exoplayer.mediacodec.MediaCodecUtil;
import androidx.media3.exoplayer.source.MediaSource;
import androidx.media3.exoplayer.source.ProgressiveMediaSource;
import androidx.media3.exoplayer.trackselection.DefaultTrackSelector;
import androidx.media3.exoplayer.upstream.DefaultBandwidthMeter;
import androidx.media3.exoplayer.upstream.DefaultLoadErrorHandlingPolicy;
import androidx.media3.datasource.DataSpec;

import org.libsdl.app.SDL;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

/**
 * One {@code <video>} element's player on Android: Media3 ExoPlayer, behind
 * runtime/core/src/media/MediaPlayer.h through runtime/core/src/media/
 * MediaPlayerAndroid.cpp, which calls the public methods here by name and
 * registers the {@code native*} callbacks in JNI_OnLoad.
 *
 * <p><b>Threads.</b> Every public method only posts: ExoPlayer runs on a looper
 * of its own ({@code screenkit.media}), never the main thread, so no call --
 * {@link #shutdown} least of all -- waits on the UI thread, which may itself be
 * blocked joining the JS thread at teardown. The plane's {@code SurfaceView}
 * lives on the UI thread and is placed there asynchronously. Licence requests
 * block ExoPlayer's DRM thread until JS answers with {@link #provideLicence}.
 *
 * <p><b>Events</b> are the MediaSink contract, each stamped with the serial of
 * the load that produced it: the player thread's {@code serial} is only changed
 * there, between one load's last event and the next load's first.
 */
@OptIn(markerClass = UnstableApi.class)
public final class VideoPlayer {
    private static final String TAG = "ScreenKit";

    // PlaybackState, as MediaPlayer.h numbers it.
    static final int STATE_LOADING = 0;
    static final int STATE_BUFFERING = 1;
    static final int STATE_READY = 2;
    static final int STATE_ENDED = 3;

    // MediaErrorKind. A wire format: MediaPlayer.h pins the same numbers, and
    // MediaPlayerAndroid.cpp asserts ERR_COUNT against kMediaErrorKindCount.
    static final int ERR_NETWORK = 0;
    static final int ERR_MANIFEST = 1;
    static final int ERR_MEDIA = 2;
    static final int ERR_VIDEO_OUTPUT = 3;
    static final int ERR_KEY_SYSTEM = 4;
    static final int ERR_LICENCE = 5;
    static final int ERR_UNAVAILABLE = 6;
    public static final int ERR_COUNT = 7;

    private static final String WIDEVINE = "com.widevine.alpha";
    private static final String CLEARKEY = "org.w3.clearkey";
    private static final long LICENCE_TIMEOUT_MS = 30000;
    private static final int TICK_MS = 250;

    // ---- the native half ------------------------------------------------------------
    private static native void nativeMetadata(long handle, int serial, double duration, boolean live,
                                              double seekStart, double seekEnd, int width, int height,
                                              boolean hasVideo, boolean hasAudio, byte[] manifest);
    private static native void nativeState(long handle, int serial, int state);
    private static native void nativeTime(long handle, int serial, double position, double seekStart,
                                          double seekEnd);
    private static native void nativeSeeked(long handle, int serial, double position);
    private static native void nativeBuffered(long handle, int serial, double[] ranges);
    private static native void nativeTracks(long handle, int serial, int[] variantInts, double[] variantDoubles,
                                            byte[][] variantStrings, int[] audioInts, byte[][] audioStrings,
                                            int[] textInts, byte[][] textStrings);
    private static native void nativeVariant(long handle, int serial, int id);
    private static native void nativeSize(long handle, int serial, int width, int height);
    private static native void nativeCues(long handle, int serial, int track, double[] times, byte[][] texts);
    private static native void nativeLicence(long handle, int serial, long requestId, byte[] keySystem,
                                             byte[] challenge);
    private static native void nativeStats(long handle, int serial, int width, int height, double frameRate,
                                           double streamBandwidth, double estimatedBandwidth, long decoded,
                                           long dropped, long corrupted);
    private static native void nativeError(long handle, int serial, int kind, int httpStatus, byte[] message);

    private final long handle;
    private final HandlerThread thread;
    private final Handler handler;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Context context;

    // ---- player-thread state -----------------------------------------------------------
    private ExoPlayer player;
    /// What the page asked for, which a playbackRate of 0 suspends without
    /// pausing the element (HTMLMediaElement: the picture freezes, `paused`
    /// stays false -- what AVPlayer does with rate 0, which ExoPlayer refuses).
    private boolean wantsPlay;
    private boolean rateIsZero;
    private DefaultBandwidthMeter meter;
    private DefaultTrackSelector selector;
    private int serial;
    private boolean loaded;
    private boolean failed;
    private boolean readyOnce;
    private boolean seekPending;
    private boolean metadataSent;
    private double lastDuration = Double.NaN;
    private boolean lastLive;
    private String manifest = "progressive";
    private int tick;
    private boolean ticking;
    private double volume = 1;
    private boolean muted;
    private int lastVariant = -1;
    private int lastState = -1;
    private int lastWidth = -1;
    private int lastHeight = -1;
    private int selectedText = -1;
    // The active cues' texts and when each first showed: ExoPlayer hands over
    // what is active, never a cue's own times, so a cue keeps the start it had
    // when it appeared for as long as it stays.
    private Map<String, Double> cueStarts = new HashMap<>();
    private ImageReader headless;
    private Surface viewSurface;
    // The tracks as JS knows them: stable ids for this load.
    private final Map<String, Integer> variantIds = new HashMap<>();
    private final Map<String, Integer> audioIds = new HashMap<>();
    private final Map<String, Integer> textIds = new HashMap<>();
    private final List<Variant> variants = new ArrayList<>();
    private final Map<Integer, TrackRef> audioRefs = new HashMap<>();
    private final Map<Integer, TrackRef> textRefs = new HashMap<>();

    // ---- licence requests, any thread ----------------------------------------------------
    private final Object licenceLock = new Object();
    private final Map<Long, byte[][]> licences = new HashMap<>();
    private long nextRequest = 1;
    private boolean cancelled;
    private volatile boolean stopped;
    /** The current load has a key system: read by {@link #decoders} on ExoPlayer's thread. */
    private volatile boolean encryptedLoad;

    // ---- UI-thread state -------------------------------------------------------------------
    private SurfaceView view;
    private boolean planeVisible;
    private double planeX, planeY, planeW, planeH;
    private int drawableW, drawableH;
    private volatile int videoW, videoH;
    private volatile float pixelRatio = 1f;

    /** The native player this one reports to; {@code handle} names it in every callback. */
    public VideoPlayer(long handle) {
        this.handle = handle;
        Context given = SDL.getContext();
        this.context = given != null ? given.getApplicationContext() : null;
        thread = new HandlerThread("screenkit.media");
        thread.start();
        handler = new Handler(thread.getLooper());
        handler.post(this::create);
        VideoLayer.videoCreated();
    }

    // ---- the seam, called from any thread --------------------------------------------------

    public void load(int serial, byte[] url, double startTime, byte[] mimeType, byte[] keySystem,
                     byte[] licenceServer, byte[] clearKeysJson, byte[] serverCertificate, byte[] audioLanguage,
                     byte[] textLanguage, double[] abr) {
        final Load load = new Load();
        load.serial = serial;
        load.url = text(url);
        load.startTime = startTime;
        load.mimeType = text(mimeType);
        load.keySystem = text(keySystem);
        load.licenceServer = text(licenceServer);
        load.clearKeysJson = clearKeysJson;
        load.serverCertificate = serverCertificate;
        load.audioLanguage = text(audioLanguage);
        load.textLanguage = text(textLanguage);
        load.abr = abr;
        cancelLicences();
        post(() -> doLoad(load));
    }

    public void play() {
        post(() -> {
            if (player == null) return;
            wantsPlay = true;
            // A rate of 0 freezes the picture while the element stays unpaused,
            // so it is play-when-ready that holds it (see setRate).
            player.setPlayWhenReady(!rateIsZero);
        });
    }

    public void pause() {
        post(() -> {
            if (player == null) return;
            wantsPlay = false;
            player.setPlayWhenReady(false);
            if (loaded) sendTime();
        });
    }

    public void seek(double position) {
        post(() -> doSeek(position));
    }

    public void setRate(double rate) {
        post(() -> {
            if (player == null) return;
            // A rate of 0 freezes the picture with `paused` still false, which is
            // what HTMLMediaElement says and what AVPlayer does; ExoPlayer refuses
            // 0, so it is `playWhenReady` that holds it instead.
            rateIsZero = rate == 0;
            if (rate > 0) {
                player.setPlaybackParameters(new PlaybackParameters((float) rate));
                player.setPlayWhenReady(wantsPlay);
            } else {
                player.setPlayWhenReady(false);
            }
        });
    }

    public void setVolume(double value) {
        post(() -> { volume = value; applyVolume(); });
    }

    public void setMuted(boolean value) {
        post(() -> { muted = value; applyVolume(); });
    }

    public void setPlane(double x, double y, double width, double height, boolean visible, int order,
                         int drawableWidth, int drawableHeight) {
        ui.post(() -> placePlane(x, y, width, height, visible, drawableWidth, drawableHeight));
    }

    public void selectVariant(int id) {
        post(() -> doSelectVariant(id));
    }

    public void setAbr(double[] abr) {
        post(() -> applyAbr(abr));
    }

    public void selectAudioLanguage(byte[] language, byte[] role) {
        final String lang = text(language);
        final String r = text(role);
        post(() -> {
            if (player == null) return;
            TrackSelectionParameters.Builder b = player.getTrackSelectionParameters().buildUpon()
                    .clearOverridesOfType(C.TRACK_TYPE_AUDIO)
                    .setPreferredAudioLanguage(lang.isEmpty() ? null : lang)
                    .setPreferredAudioRoleFlags(roleFlags(r));
            player.setTrackSelectionParameters(b.build());
        });
    }

    public void selectText(int id) {
        post(() -> doSelectText(id));
    }

    /** JS's answer to a licence request: the licence server's bytes, or null when the exchange failed. */
    public void provideLicence(long requestId, byte[] licence) {
        synchronized (licenceLock) {
            if (!licences.containsKey(requestId)) return;
            licences.put(requestId, new byte[][] {licence == null ? new byte[0] : licence});
            licenceLock.notifyAll();
        }
    }

    public void unload() {
        cancelLicences();
        post(() -> {
            if (player == null) return;
            player.stop();
            player.clearMediaItems();
            loaded = false;
            stopTicking();
        });
    }

    /** Releases everything, asynchronously; the native side has already stopped listening. */
    public void shutdown() {
        stopped = true;
        synchronized (licenceLock) {
            cancelled = true;
            licenceLock.notifyAll();
        }
        ui.post(this::removePlane);
        handler.post(() -> {
            stopTicking();
            if (player != null) {
                player.release();
                player = null;
            }
            if (headless != null) {
                headless.close();
                headless = null;
            }
            thread.quitSafely();
        });
    }

    // ---- what this device can play --------------------------------------------------------

    /**
     * "platform:android", then "keySystem:<name>" for each key system MediaDrm
     * supports and "codec:<prefix>" for each RFC 6381 codec a decoder exists for.
     */
    public static String[] capabilities() {
        Set<String> out = new LinkedHashSet<>();
        try {
            if (MediaDrm.isCryptoSchemeSupported(C.WIDEVINE_UUID)) out.add("keySystem:" + WIDEVINE);
        } catch (Throwable ignored) {
        }
        try {
            if (MediaDrm.isCryptoSchemeSupported(C.CLEARKEY_UUID)) out.add("keySystem:" + CLEARKEY);
        } catch (Throwable ignored) {
        }
        Map<String, String[]> codecs = new HashMap<>();
        codecs.put(MimeTypes.VIDEO_H264, new String[] {"avc1", "avc3"});
        codecs.put(MimeTypes.VIDEO_H265, new String[] {"hvc1", "hev1"});
        codecs.put(MimeTypes.VIDEO_VP8, new String[] {"vp8"});
        codecs.put(MimeTypes.VIDEO_VP9, new String[] {"vp09", "vp9"});
        codecs.put(MimeTypes.VIDEO_AV1, new String[] {"av01"});
        codecs.put(MimeTypes.VIDEO_DOLBY_VISION, new String[] {"dvh1", "dvhe"});
        codecs.put(MimeTypes.AUDIO_AAC, new String[] {"mp4a"});
        codecs.put(MimeTypes.AUDIO_AC3, new String[] {"ac-3"});
        codecs.put(MimeTypes.AUDIO_E_AC3, new String[] {"ec-3"});
        codecs.put(MimeTypes.AUDIO_OPUS, new String[] {"opus"});
        codecs.put(MimeTypes.AUDIO_FLAC, new String[] {"flac"});
        codecs.put(MimeTypes.AUDIO_MPEG, new String[] {"mp3", "mp4a.40.34"});
        codecs.put(MimeTypes.AUDIO_VORBIS, new String[] {"vorbis"});
        try {
            for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
                if (info.isEncoder()) continue;
                for (String type : info.getSupportedTypes()) {
                    String[] names = codecs.get(type.toLowerCase());
                    if (names == null) continue;
                    for (String name : names) out.add("codec:" + name);
                }
            }
        } catch (Throwable e) {
            Log.w(TAG, "could not list the device's decoders", e);
        }
        return out.toArray(new String[0]);
    }

    // ---- player thread ------------------------------------------------------------------------

    private void post(Runnable work) {
        if (stopped) return;
        handler.post(() -> {
            if (!stopped) work.run();
        });
    }

    private void create() {
        if (context == null) {
            Log.e(TAG, "media: no Context -- SDL has not been given one");
            return;
        }
        meter = new DefaultBandwidthMeter.Builder(context).build();
        selector = new DefaultTrackSelector(context);
        player = new ExoPlayer.Builder(context, new DefaultRenderersFactory(context).setMediaCodecSelector(this::decoders))
                .setLooper(thread.getLooper())
                .setBandwidthMeter(meter)
                .setTrackSelector(selector)
                // Audio focus and "becoming noisy" would pause behind the page's
                // back; `paused` is the page's (MediaPlayer.h).
                .setHandleAudioBecomingNoisy(false)
                // ExoPlayer reports a player error when a renderer takes longer
                // than this to release, and its default is 500 ms. On a busy
                // device that is a teardown reported to the page as a failed
                // load: seen on the emulator running the whole suite, where
                // media-shutdown's players failed with "Player release timed
                // out" while the next rows still ran.
                .setReleaseTimeoutMs(5_000)
                .build();
        player.setTrackSelectionParameters(player.getTrackSelectionParameters().buildUpon()
                .setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true).build());
        player.addListener(new Listener());
        player.addAnalyticsListener(new Analytics());
        useHeadlessSurface();
    }

    /**
     * Somewhere to decode into while no plane is showing -- the test harness has
     * no layout at all -- so frames are still decoded and counted: an ImageReader
     * whose images are dropped as they arrive.
     */
    private void useHeadlessSurface() {
        if (player == null) return;
        if (headless == null) {
            headless = ImageReader.newInstance(1280, 720, ImageFormat.PRIVATE, 4);
            headless.setOnImageAvailableListener(reader -> {
                Image image = reader.acquireLatestImage();
                if (image != null) image.close();
            }, handler);
        }
        player.setVideoSurface(headless.getSurface());
    }

    private void doLoad(Load load) {
        if (player == null) {
            nativeError(handle, load.serial, ERR_UNAVAILABLE, 0, bytes("the media player could not start"));
            return;
        }
        // The old load's last events go out under its own serial.
        stopTicking();
        player.stop();
        player.clearMediaItems();
        serial = load.serial;
        loaded = true;
        failed = false;
        readyOnce = false;
        seekPending = false;
        metadataSent = false;
        lastDuration = Double.NaN;
        lastLive = false;
        lastVariant = -1;
        lastState = -1;
        lastWidth = -1;
        lastHeight = -1;
        selectedText = -1;
        cueStarts = new HashMap<>();
        variantIds.clear();
        audioIds.clear();
        textIds.clear();
        variants.clear();
        audioRefs.clear();
        textRefs.clear();
        synchronized (licenceLock) {
            cancelled = false;
        }

        Uri uri = Uri.parse(load.url);
        int type = Util.inferContentTypeForUriAndMimeType(uri, mimeOf(load.mimeType));
        manifest = type == C.CONTENT_TYPE_HLS ? "hls" : type == C.CONTENT_TYPE_DASH ? "dash" : "progressive";
        if (type == C.CONTENT_TYPE_SS || type == C.CONTENT_TYPE_RTSP) {
            fail(ERR_MANIFEST, 0, "not a manifest type this player plays: " + load.url);
            return;
        }

        DrmSessionManager drm;
        try {
            drm = drmFor(load);
        } catch (KeySystemException e) {
            fail(ERR_KEY_SYSTEM, 0, e.getMessage());
            return;
        }
        encryptedLoad = !load.keySystem.isEmpty();

        DefaultHttpDataSource.Factory http = new DefaultHttpDataSource.Factory()
                .setAllowCrossProtocolRedirects(true)
                .setUserAgent("Mozilla/5.0 (ScreenKit)");
        DataSource.Factory data = new DefaultDataSource.Factory(context, http);
        MediaItem item = new MediaItem.Builder().setUri(uri).setMimeType(mimeOf(load.mimeType)).build();
        MediaSource source;
        if (type == C.CONTENT_TYPE_HLS) {
            source = new HlsMediaSource.Factory(data).setDrmSessionManagerProvider(i -> drm).createMediaSource(item);
        } else if (type == C.CONTENT_TYPE_DASH) {
            source = new DashMediaSource.Factory(data).setDrmSessionManagerProvider(i -> drm).createMediaSource(item);
        } else {
            source = new ProgressiveMediaSource.Factory(data).setDrmSessionManagerProvider(i -> drm)
                    .createMediaSource(item);
        }

        TrackSelectionParameters.Builder params = player.getTrackSelectionParameters().buildUpon()
                .clearOverrides()
                .setPreferredAudioLanguage(load.audioLanguage.isEmpty() ? null : load.audioLanguage)
                .setPreferredTextLanguage(load.textLanguage.isEmpty() ? null : load.textLanguage)
                // Text stays off until selectText: the page's TextTracks start
                // disabled, and the Shaka layer selects its preferred one itself.
                .setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true);
        player.setTrackSelectionParameters(params.build());
        applyAbr(load.abr);

        player.setPlayWhenReady(false);
        // A new load starts paused and at the ordinary rate, as the element does.
        wantsPlay = false;
        rateIsZero = false;
        player.setPlaybackParameters(PlaybackParameters.DEFAULT);
        if (!Double.isNaN(load.startTime) && load.startTime > 0) {
            player.setMediaSource(source, (long) (load.startTime * 1000));
        } else {
            player.setMediaSource(source);
        }
        player.prepare();
        sendState(STATE_LOADING);
    }

    private static String mimeOf(String given) {
        if (given == null || given.isEmpty()) return null;
        String m = given.toLowerCase();
        if (m.contains("mpegurl")) return MimeTypes.APPLICATION_M3U8;
        if (m.contains("dash")) return MimeTypes.APPLICATION_MPD;
        return m;
    }

    private static final class KeySystemException extends Exception {
        KeySystemException(String message) {
            super(message);
        }
    }

    private DrmSessionManager drmFor(Load load) throws KeySystemException {
        String system = load.keySystem;
        if (system.isEmpty()) return DrmSessionManager.DRM_UNSUPPORTED;
        UUID uuid;
        if (system.equals(WIDEVINE)) {
            uuid = C.WIDEVINE_UUID;
        } else if (system.equals(CLEARKEY)) {
            uuid = C.CLEARKEY_UUID;
        } else {
            throw new KeySystemException("the key system " + system + " is not available on Android");
        }
        boolean supported;
        try {
            supported = MediaDrm.isCryptoSchemeSupported(uuid);
        } catch (Throwable e) {
            supported = false;
        }
        if (!supported) throw new KeySystemException("this device has no " + system + " CDM");
        MediaDrmCallback callback;
        if (uuid.equals(C.CLEARKEY_UUID) && load.clearKeysJson != null && load.clearKeysJson.length > 0) {
            // drm.clearKeys: the keys are the licence, and nothing is fetched.
            callback = new LocalMediaDrmCallback(load.clearKeysJson);
        } else {
            callback = new JsDrmCallback(load.serial, system);
        }
        // drm.advanced.serverCertificate: Widevine's service certificate, which a
        // licence server may require before it will answer a challenge. Handed to
        // the CDM here, since ExoPlayer has no place for it on the manager.
        final byte[] certificate = load.serverCertificate;
        ExoMediaDrm.Provider drmProvider = FrameworkMediaDrm.DEFAULT_PROVIDER;
        if (certificate != null && certificate.length > 0) {
            drmProvider = drmUuid -> {
                ExoMediaDrm drm = FrameworkMediaDrm.DEFAULT_PROVIDER.acquireExoMediaDrm(drmUuid);
                try {
                    drm.setPropertyByteArray("serviceCertificate", certificate);
                } catch (Exception e) {
                    // A CDM that has no such property says so; the licence
                    // exchange then goes on without it, as it did before.
                    Log.w(TAG, "the key system refused the service certificate: " + e);
                }
                return drm;
            };
        }
        DefaultDrmSessionManager.Builder builder = new DefaultDrmSessionManager.Builder()
                .setUuidAndExoMediaDrmProvider(uuid, drmProvider)
                .setMultiSession(false)
                // No retries here. A failed licence request is ExoPlayer's to
                // report, not to repeat: the exchange runs in JS, where Shaka's
                // drm.retryParameters and the request filters decide how often
                // the licence server is asked. ExoPlayer's default policy retries
                // a key request three times behind Shaka's back (the first after
                // no delay at all), which sends a second challenge to the licence
                // server and races the teardown of the failed load.
                .setLoadErrorHandlingPolicy(new DefaultLoadErrorHandlingPolicy(0));
        return builder.build(callback);
    }

    /**
     * The decoders for a stream, ExoPlayer's own list -- except that an encrypted
     * load passes over the Android emulator's host-backed decoders
     * ({@code c2.goldfish.*}): they take the decrypted input and never produce a
     * frame, so a ClearKey stream would buffer for ever. The emulator's software
     * decoders ({@code c2.android.*}) take it. No device carries a goldfish codec.
     */
    private List<androidx.media3.exoplayer.mediacodec.MediaCodecInfo> decoders(
            String mimeType, boolean requiresSecureDecoder, boolean requiresTunneling)
            throws MediaCodecUtil.DecoderQueryException {
        List<androidx.media3.exoplayer.mediacodec.MediaCodecInfo> all =
                MediaCodecSelector.DEFAULT.getDecoderInfos(mimeType, requiresSecureDecoder, requiresTunneling);
        if (!encryptedLoad) return all;
        List<androidx.media3.exoplayer.mediacodec.MediaCodecInfo> usable = new ArrayList<>();
        for (androidx.media3.exoplayer.mediacodec.MediaCodecInfo info : all) {
            if (!info.name.startsWith("c2.goldfish.")) usable.add(info);
        }
        return usable.isEmpty() ? all : usable;
    }

    /**
     * A licence request goes to JS, which runs Shaka's request filters, fetches
     * it and answers through provideLicence. This runs on ExoPlayer's DRM thread,
     * which blocks here until then.
     */
    private final class JsDrmCallback implements MediaDrmCallback {
        private final int loadSerial;
        private final String keySystem;

        JsDrmCallback(int loadSerial, String keySystem) {
            this.loadSerial = loadSerial;
            this.keySystem = keySystem;
        }

        @Override
        public MediaDrmCallback.Response executeProvisionRequest(UUID uuid, ExoMediaDrm.ProvisionRequest request)
                throws MediaDrmCallbackException {
            // Device provisioning, not a licence: the CDM's own request to its
            // vendor's provisioning server, made here as ExoPlayer's
            // HttpMediaDrmCallback makes it. A provisioned device never gets here.
            String url = request.getDefaultUrl() + "&signedRequest="
                    + new String(request.getData(), StandardCharsets.UTF_8);
            try {
                return new MediaDrmCallback.Response(post(url, new byte[0]));
            } catch (IOException e) {
                throw new MediaDrmCallbackException(new DataSpec(Uri.parse(url)), Uri.parse(url),
                        Collections.emptyMap(), 0, e);
            }
        }

        @Override
        public MediaDrmCallback.Response executeKeyRequest(UUID uuid, ExoMediaDrm.KeyRequest request)
                throws MediaDrmCallbackException {
            long id;
            synchronized (licenceLock) {
                if (cancelled || stopped) throw licenceFailure("the load was replaced");
                id = nextRequest++;
                licences.put(id, null);
            }
            nativeLicence(handle, loadSerial, id, bytes(keySystem), request.getData());
            byte[] answer;
            synchronized (licenceLock) {
                long deadline = System.currentTimeMillis() + LICENCE_TIMEOUT_MS;
                while (licences.get(id) == null && !cancelled && !stopped) {
                    long left = deadline - System.currentTimeMillis();
                    if (left <= 0) break;
                    try {
                        licenceLock.wait(left);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        break;
                    }
                }
                byte[][] slot = licences.remove(id);
                answer = slot == null ? null : slot[0];
            }
            if (answer == null || answer.length == 0) {
                throw licenceFailure(answer == null ? "no licence within " + LICENCE_TIMEOUT_MS + " ms"
                                                    : "the licence request failed");
            }
            return new MediaDrmCallback.Response(answer);
        }

        private MediaDrmCallbackException licenceFailure(String why) {
            return new MediaDrmCallbackException(new DataSpec(Uri.EMPTY), Uri.EMPTY, Collections.emptyMap(), 0,
                    new IOException(why));
        }
    }

    private static byte[] post(String url, byte[] body) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        try {
            connection.setRequestMethod("POST");
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/json");
            try (OutputStream out = connection.getOutputStream()) {
                out.write(body);
            }
            int status = connection.getResponseCode();
            if (status < 200 || status > 299) throw new IOException("HTTP " + status);
            try (InputStream in = connection.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
                return out.toByteArray();
            }
        } finally {
            connection.disconnect();
        }
    }

    private void cancelLicences() {
        synchronized (licenceLock) {
            cancelled = true;
            licenceLock.notifyAll();
        }
    }

    private void doSeek(double position) {
        if (player == null || !loaded || failed) return;
        long windowStartMs = (long) (windowStart() * 1000);
        long target = Math.max(0, (long) (position * 1000) - windowStartMs);
        seekPending = true;
        player.seekTo(target);
    }

    private void applyVolume() {
        if (player != null) player.setVolume(muted ? 0f : (float) volume);
    }

    // ---- positions: period time, which advances with a live window ---------------------------

    private final Timeline.Window window = new Timeline.Window();

    /** Where the current window starts in its period, seconds; 0 for VOD. */
    private double windowStart() {
        Timeline timeline = player.getCurrentTimeline();
        if (timeline.isEmpty()) return 0;
        timeline.getWindow(player.getCurrentMediaItemIndex(), window);
        long us = window.getPositionInFirstPeriodUs();
        return us == C.TIME_UNSET ? 0 : us / 1e6;
    }

    private boolean live() {
        Timeline timeline = player.getCurrentTimeline();
        if (timeline.isEmpty()) return false;
        timeline.getWindow(player.getCurrentMediaItemIndex(), window);
        return window.isLive() || (window.isDynamic && window.durationUs == C.TIME_UNSET);
    }

    private double windowDuration() {
        long ms = player.getDuration();
        return ms == C.TIME_UNSET ? Double.NaN : ms / 1000.0;
    }

    private double position() {
        double at = windowStart() + player.getCurrentPosition() / 1000.0;
        // ExoPlayer's position runs a frame or two past a VOD's end.
        double duration = windowDuration();
        return !Double.isNaN(duration) && !live() ? Math.min(at, duration) : at;
    }

    /** One state event per change: ExoPlayer's IDLE -> BUFFERING both read as "loading". */
    private void sendState(int state) {
        if (state == lastState) return;
        lastState = state;
        nativeState(handle, serial, state);
    }

    private void sendTime() {
        if (player == null || !loaded) return;
        double start = windowStart();
        double duration = windowDuration();
        double end = Double.isNaN(duration) ? start : start + duration;
        nativeTime(handle, serial, position(), start, end);
    }

    private void sendMetadata() {
        if (player == null || !loaded) return;
        boolean isLive = live();
        double duration = isLive ? Double.POSITIVE_INFINITY : windowDuration();
        if (metadataSent && isLive == lastLive
                && (duration == lastDuration || (Double.isNaN(duration) && Double.isNaN(lastDuration)))) {
            return;
        }
        metadataSent = true;
        lastLive = isLive;
        lastDuration = duration;
        double start = windowStart();
        double windowLength = windowDuration();
        double end = Double.isNaN(windowLength) ? start : start + windowLength;
        Tracks tracks = player.getCurrentTracks();
        VideoSize size = player.getVideoSize();
        int width = size.width, height = size.height;
        Format video = player.getVideoFormat();
        if ((width == 0 || height == 0) && video != null) {
            width = video.width;
            height = video.height;
        }
        nativeMetadata(handle, serial, duration, isLive, start, end, Math.max(0, width), Math.max(0, height),
                tracks.containsType(C.TRACK_TYPE_VIDEO), tracks.containsType(C.TRACK_TYPE_AUDIO),
                bytes(manifest));
    }

    private void startTicking() {
        if (ticking) return;
        ticking = true;
        handler.postDelayed(ticker, TICK_MS);
    }

    private void stopTicking() {
        ticking = false;
        handler.removeCallbacks(ticker);
    }

    private final Runnable ticker = new Runnable() {
        @Override
        public void run() {
            if (!ticking || player == null || stopped) return;
            sendTime();
            sendBuffered();
            if (++tick % 4 == 0) sendStats();
            handler.postDelayed(this, TICK_MS);
        }
    };

    private void sendBuffered() {
        if (player == null || !loaded) return;
        double start = windowStart();
        double from = start + player.getCurrentPosition() / 1000.0;
        double to = start + player.getBufferedPosition() / 1000.0;
        nativeBuffered(handle, serial, to > from ? new double[] {from, to} : new double[0]);
    }

    private void sendStats() {
        if (player == null || !loaded) return;
        Format video = player.getVideoFormat();
        Format audio = player.getAudioFormat();
        DecoderCounters counters = player.getVideoDecoderCounters();
        long decoded = 0, dropped = 0;
        if (counters != null) {
            counters.ensureUpdated();
            decoded = counters.renderedOutputBufferCount + counters.skippedOutputBufferCount
                    + counters.droppedBufferCount;
            dropped = counters.droppedBufferCount;
        }
        double stream = Double.NaN;
        if (video != null && video.bitrate != Format.NO_VALUE) stream = video.bitrate;
        if (audio != null && audio.bitrate != Format.NO_VALUE) {
            stream = Double.isNaN(stream) ? audio.bitrate : stream + audio.bitrate;
        }
        long estimate = meter != null ? meter.getBitrateEstimate() : 0;
        VideoSize size = player.getVideoSize();
        nativeStats(handle, serial, size.width, size.height,
                video != null && video.frameRate != Format.NO_VALUE ? video.frameRate : 0,
                stream, estimate > 0 ? estimate : Double.NaN, decoded, dropped, 0);
    }

    private void fail(int kind, int status, String message) {
        if (failed) return;
        failed = true;
        Log.w(TAG, "media: load " + serial + " failed (kind " + kind + (status > 0 ? ", HTTP " + status : "")
                + "): " + message);
        stopTicking();
        cancelLicences();
        nativeError(handle, serial, kind, status, bytes(message == null ? "playback failed" : message));
    }

    // ---- tracks: Shaka's variants, over ExoPlayer's groups -----------------------------------

    private static final class TrackRef {
        final Tracks.Group group;
        final int index;

        TrackRef(Tracks.Group group, int index) {
            this.group = group;
            this.index = index;
        }
    }

    private static final class Variant {
        int id;
        TrackRef video;
        TrackRef audio;
        int audioId = -1;
    }

    private static String key(Tracks.Group group, int index) {
        return group.getMediaTrackGroup().id + "/" + index;
    }

    private void rebuildTracks(Tracks tracks) {
        List<TrackRef> videoRefs = new ArrayList<>();
        List<TrackRef> audioList = new ArrayList<>();
        variants.clear();
        audioRefs.clear();
        textRefs.clear();
        for (Tracks.Group group : tracks.getGroups()) {
            for (int i = 0; i < group.length; i++) {
                if (!group.isTrackSupported(i, /* allowExceedsCapabilities= */ true)) continue;
                TrackRef ref = new TrackRef(group, i);
                switch (group.getType()) {
                    case C.TRACK_TYPE_VIDEO:
                        videoRefs.add(ref);
                        break;
                    case C.TRACK_TYPE_AUDIO: {
                        audioList.add(ref);
                        String k = key(group, i);
                        Integer id = audioIds.get(k);
                        if (id == null) audioIds.put(k, id = audioIds.size());
                        audioRefs.put(id, ref);
                        break;
                    }
                    case C.TRACK_TYPE_TEXT: {
                        String k = key(group, i);
                        Integer id = textIds.get(k);
                        if (id == null) textIds.put(k, id = textIds.size());
                        textRefs.put(id, ref);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        // Every video with every audio, as Shaka lists a DASH period's variants;
        // with only one kind, one variant per track of it.
        if (videoRefs.isEmpty()) videoRefs.add(null);
        if (audioList.isEmpty()) audioList.add(null);
        for (TrackRef v : videoRefs) {
            for (TrackRef a : audioList) {
                if (v == null && a == null) continue;
                String k = (v == null ? "-" : key(v.group, v.index)) + "|" + (a == null ? "-" : key(a.group, a.index));
                Integer id = variantIds.get(k);
                if (id == null) variantIds.put(k, id = variantIds.size());
                Variant variant = new Variant();
                variant.id = id;
                variant.video = v;
                variant.audio = a;
                variant.audioId = a == null ? -1 : audioIds.get(key(a.group, a.index));
                variants.add(variant);
            }
        }
    }

    private static boolean same(Format a, Format b) {
        if (a == null || b == null) return false;
        if (a.id != null && b.id != null) return a.id.equals(b.id);
        return a.bitrate == b.bitrate && a.width == b.width && a.height == b.height;
    }

    /** The variant playing now: the formats the renderers have, not the adaptive set. */
    private int activeVariant() {
        Format video = player.getVideoFormat();
        Format audio = player.getAudioFormat();
        int fallback = -1;
        for (Variant v : variants) {
            boolean videoMatches = v.video == null ? video == null : same(v.video.group.getTrackFormat(v.video.index), video);
            boolean audioMatches = v.audio == null || audio == null
                    || same(v.audio.group.getTrackFormat(v.audio.index), audio)
                    || v.audio.group.isTrackSelected(v.audio.index);
            if (videoMatches && audioMatches) return v.id;
            if (videoMatches && fallback < 0) fallback = v.id;
        }
        return fallback;
    }

    private static String nonNull(String s) {
        return s == null ? "" : s;
    }

    private static String role(int flags) {
        if ((flags & C.ROLE_FLAG_MAIN) != 0) return "main";
        if ((flags & C.ROLE_FLAG_ALTERNATE) != 0) return "alternate";
        if ((flags & C.ROLE_FLAG_COMMENTARY) != 0) return "commentary";
        if ((flags & C.ROLE_FLAG_DESCRIBES_VIDEO) != 0) return "description";
        if ((flags & C.ROLE_FLAG_DUB) != 0) return "dub";
        return "";
    }

    private static int roleFlags(String role) {
        switch (role) {
            case "main": return C.ROLE_FLAG_MAIN;
            case "alternate": return C.ROLE_FLAG_ALTERNATE;
            case "commentary": return C.ROLE_FLAG_COMMENTARY;
            case "description": return C.ROLE_FLAG_DESCRIBES_VIDEO;
            case "dub": return C.ROLE_FLAG_DUB;
            default: return 0;
        }
    }

    private void sendTracks() {
        if (player == null || !loaded) return;
        int active = activeVariant();
        int n = variants.size();
        int[] vi = new int[n * 6];
        double[] vd = new double[n * 2];
        byte[][] vs = new byte[n * 4][];
        for (int i = 0; i < n; i++) {
            Variant v = variants.get(i);
            Format video = v.video == null ? null : v.video.group.getTrackFormat(v.video.index);
            Format audio = v.audio == null ? null : v.audio.group.getTrackFormat(v.audio.index);
            double bandwidth = 0;
            if (video != null && video.bitrate != Format.NO_VALUE) bandwidth += video.bitrate;
            if (audio != null && audio.bitrate != Format.NO_VALUE) bandwidth += audio.bitrate;
            vi[i * 6] = v.id;
            vi[i * 6 + 1] = video == null || video.width == Format.NO_VALUE ? 0 : video.width;
            vi[i * 6 + 2] = video == null || video.height == Format.NO_VALUE ? 0 : video.height;
            vi[i * 6 + 3] = v.audioId;
            vi[i * 6 + 4] = channels(audio);
            vi[i * 6 + 5] = v.id == active ? 1 : 0;
            vd[i * 2] = bandwidth;
            vd[i * 2 + 1] = video == null || video.frameRate == Format.NO_VALUE ? 0 : video.frameRate;
            vs[i * 4] = bytes(video == null ? "" : nonNull(video.codecs));
            vs[i * 4 + 1] = bytes(audio == null ? "" : nonNull(audio.codecs));
            vs[i * 4 + 2] = bytes(audio == null ? "" : nonNull(audio.language));
            vs[i * 4 + 3] = bytes(video != null ? nonNull(video.label) : audio == null ? "" : nonNull(audio.label));
        }
        Format playingAudio = player.getAudioFormat();
        int an = audioRefs.size();
        int[] ai = new int[an * 3];
        byte[][] as = new byte[an * 4][];
        int j = 0;
        for (Map.Entry<Integer, TrackRef> e : audioRefs.entrySet()) {
            Format f = e.getValue().group.getTrackFormat(e.getValue().index);
            ai[j * 3] = e.getKey();
            ai[j * 3 + 1] = channels(f);
            ai[j * 3 + 2] = same(f, playingAudio) || (playingAudio != null && an == 1) ? 1 : 0;
            as[j * 4] = bytes(nonNull(f.language));
            as[j * 4 + 1] = bytes(nonNull(f.label));
            as[j * 4 + 2] = bytes(role(f.roleFlags));
            as[j * 4 + 3] = bytes(nonNull(f.codecs));
            j++;
        }
        int tn = textRefs.size();
        int[] ti = new int[tn * 3];
        byte[][] ts = new byte[tn * 4][];
        j = 0;
        for (Map.Entry<Integer, TrackRef> e : textRefs.entrySet()) {
            TrackRef ref = e.getValue();
            Format f = ref.group.getTrackFormat(ref.index);
            String mime = f.sampleMimeType != null ? f.sampleMimeType : nonNull(f.containerMimeType);
            boolean captions = (f.roleFlags & C.ROLE_FLAG_CAPTION) != 0
                    || MimeTypes.APPLICATION_CEA608.equals(mime) || MimeTypes.APPLICATION_CEA708.equals(mime);
            if (MimeTypes.TEXT_VTT.equals(f.codecs) || MimeTypes.TEXT_VTT.equals(mime)
                    || MimeTypes.APPLICATION_MEDIA3_CUES.equals(mime) && MimeTypes.TEXT_VTT.equals(f.codecs)) {
                mime = MimeTypes.TEXT_VTT;
            }
            ti[j * 3] = e.getKey();
            ti[j * 3 + 1] = (f.selectionFlags & C.SELECTION_FLAG_FORCED) != 0 ? 1 : 0;
            ti[j * 3 + 2] = e.getKey() == selectedText ? 1 : 0;
            ts[j * 4] = bytes(nonNull(f.language));
            ts[j * 4 + 1] = bytes(nonNull(f.label));
            ts[j * 4 + 2] = bytes(captions ? "captions" : "subtitles");
            ts[j * 4 + 3] = bytes(mime);
            j++;
        }
        nativeTracks(handle, serial, vi, vd, vs, ai, as, ti, ts);
    }

    /** A track's channels; a muxed HLS audio track says only once it plays. */
    private int channels(Format format) {
        if (format == null) return 0;
        if (format.channelCount != Format.NO_VALUE) return format.channelCount;
        Format playing = player.getAudioFormat();
        if (playing != null && playing.channelCount != Format.NO_VALUE
                && (same(format, playing) || nonNull(format.codecs).equals(nonNull(playing.codecs)))) {
            return playing.channelCount;
        }
        return 0;
    }

    private void checkVariant() {
        if (player == null || !loaded) return;
        int active = activeVariant();
        if (active >= 0 && active != lastVariant) {
            lastVariant = active;
            sendTracks();
            nativeVariant(handle, serial, active);
        }
    }

    private void doSelectVariant(int id) {
        if (player == null) return;
        for (Variant v : variants) {
            if (v.id != id) continue;
            TrackSelectionParameters.Builder b = player.getTrackSelectionParameters().buildUpon();
            if (v.video != null) {
                b.setOverrideForType(new TrackSelectionOverride(v.video.group.getMediaTrackGroup(), v.video.index));
            }
            if (v.audio != null) {
                b.setOverrideForType(new TrackSelectionOverride(v.audio.group.getMediaTrackGroup(), v.audio.index));
            }
            player.setTrackSelectionParameters(b.build());
            return;
        }
    }

    /** [enabled, minBandwidth, maxBandwidth, minWidth, maxWidth, minHeight, maxHeight]. */
    private void applyAbr(double[] abr) {
        if (player == null || abr == null || abr.length < 7) return;
        TrackSelectionParameters.Builder b = player.getTrackSelectionParameters().buildUpon();
        if (abr[0] != 0) {
            // ABR back on: no pinned variant, and ABR chooses inside the restrictions.
            b.clearOverridesOfType(C.TRACK_TYPE_VIDEO).clearOverridesOfType(C.TRACK_TYPE_AUDIO);
        } else if (loaded) {
            // Off: keep what plays now.
            Format video = player.getVideoFormat();
            for (Variant v : variants) {
                if (v.video != null && same(v.video.group.getTrackFormat(v.video.index), video)) {
                    b.setOverrideForType(new TrackSelectionOverride(v.video.group.getMediaTrackGroup(),
                            v.video.index));
                    break;
                }
            }
        }
        b.setMinVideoBitrate(clampInt(abr[1], 0));
        b.setMaxVideoBitrate(clampInt(abr[2], Integer.MAX_VALUE));
        b.setMinVideoSize(clampInt(abr[3], 0), clampInt(abr[5], 0));
        b.setMaxVideoSize(clampInt(abr[4], Integer.MAX_VALUE), clampInt(abr[6], Integer.MAX_VALUE));
        player.setTrackSelectionParameters(b.build());
    }

    private static int clampInt(double value, int fallback) {
        if (Double.isNaN(value)) return fallback;
        if (value >= Integer.MAX_VALUE) return Integer.MAX_VALUE;
        if (value <= 0) return 0;
        return (int) value;
    }

    private void doSelectText(int id) {
        if (player == null) return;
        TrackSelectionParameters.Builder b = player.getTrackSelectionParameters().buildUpon();
        TrackRef ref = id >= 0 ? textRefs.get(id) : null;
        if (ref == null) {
            selectedText = -1;
            b.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true).clearOverridesOfType(C.TRACK_TYPE_TEXT);
        } else {
            selectedText = id;
            b.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, false)
                    .setOverrideForType(new TrackSelectionOverride(ref.group.getMediaTrackGroup(), ref.index));
        }
        player.setTrackSelectionParameters(b.build());
        cueStarts = new HashMap<>();
        sendTracks();
        // Nothing is active on a track that was just (de)selected until it says so.
        nativeCues(handle, serial, selectedText, new double[0], new byte[0][]);
    }

    // ---- listeners (player thread) --------------------------------------------------------------

    private final class Listener implements Player.Listener {
        @Override
        public void onPlaybackStateChanged(int state) {
            if (!loaded || failed) return;
            switch (state) {
                case Player.STATE_BUFFERING:
                    sendState(readyOnce ? STATE_BUFFERING : STATE_LOADING);
                    break;
                case Player.STATE_READY:
                    if (!readyOnce) {
                        readyOnce = true;
                        // Tracks, the variant and the size before the metadata:
                        // loadedmetadata is what resolves a load, and whoever
                        // awaited it reads the tracks next.
                        rebuildTracks(player.getCurrentTracks());
                        sendTracks();
                        checkVariant();
                        sendSize(player.getVideoSize());
                        // Where it starts, too: a load with a start time resolves
                        // there, not at 0.
                        sendTime();
                        sendMetadata();
                    }
                    sendState(STATE_READY);
                    if (seekPending) {
                        seekPending = false;
                        nativeSeeked(handle, serial, position());
                    }
                    sendTime();
                    sendStats();
                    break;
                case Player.STATE_ENDED:
                    if (!readyOnce) {
                        readyOnce = true;
                        sendMetadata();
                    }
                    if (seekPending) {
                        seekPending = false;
                        nativeSeeked(handle, serial, position());
                    }
                    stopTicking();
                    sendTime();
                    sendStats();
                    sendState(STATE_ENDED);
                    break;
                default:
                    break;
            }
        }

        @Override
        public void onIsPlayingChanged(boolean playing) {
            if (!loaded || failed) return;
            if (playing) startTicking(); else stopTicking();
            sendTime();
        }

        @Override
        public void onPositionDiscontinuity(Player.PositionInfo from, Player.PositionInfo to, int reason) {
            // A seek out of the buffered range goes BUFFERING -> READY and is
            // answered there. One that resolves to the millisecond already held
            // changes no state at all (ExoPlayerImplInternal.seekToInternal
            // returns early), so this is the only word of it -- without which the
            // element stays `seeking` for ever.
            if (!loaded || failed || !seekPending) return;
            if (reason != Player.DISCONTINUITY_REASON_SEEK) return;
            seekPending = false;
            nativeSeeked(handle, serial, position());
            sendTime();
        }

        @Override
        public void onTimelineChanged(Timeline timeline, int reason) {
            if (!loaded || failed || !readyOnce) return;
            sendMetadata();
        }

        @Override
        public void onTracksChanged(Tracks tracks) {
            if (!loaded || failed || !readyOnce) return;
            rebuildTracks(tracks);
            sendTracks();
            checkVariant();
        }

        @Override
        public void onVideoSizeChanged(VideoSize size) {
            if (!loaded || failed) return;
            videoW = size.width;
            videoH = size.height;
            pixelRatio = size.pixelWidthHeightRatio > 0 ? size.pixelWidthHeightRatio : 1f;
            ui.post(VideoPlayer.this::layoutPlane);
            if (readyOnce) sendSize(size);
        }

        @Override
        public void onCues(CueGroup group) {
            if (!loaded || failed || selectedText < 0) return;
            double at = windowStart() + player.getCurrentPosition() / 1000.0;
            Map<String, Double> starts = new HashMap<>();
            List<byte[]> texts = new ArrayList<>();
            List<Double> begins = new ArrayList<>();
            for (Cue cue : group.cues) {
                if (cue.text == null) continue;
                String text = cue.text.toString();
                Double start = cueStarts.get(text);
                if (start == null) start = at;
                starts.put(text, start);
                texts.add(bytes(text));
                begins.add(start);
            }
            cueStarts = starts;
            double[] times = new double[texts.size() * 2];
            for (int i = 0; i < texts.size(); i++) {
                // When it ends is not known until it has: NaN (a recorded divergence).
                times[i * 2] = begins.get(i);
                times[i * 2 + 1] = Double.NaN;
            }
            nativeCues(handle, serial, selectedText, times, texts.toArray(new byte[0][]));
        }

        @Override
        public void onPlayerError(PlaybackException error) {
            // `stopped`: what a player reports while it is being released is not
            // the page's business -- it asked for the release.
            if (!loaded || stopped) return;
            classify(error);
        }
    }

    private void sendSize(VideoSize size) {
        int width = Math.round(size.width * (size.pixelWidthHeightRatio > 0 ? size.pixelWidthHeightRatio : 1f));
        int height = size.height;
        if (width <= 0 || height <= 0 || (width == lastWidth && height == lastHeight)) return;
        lastWidth = width;
        lastHeight = height;
        nativeSize(handle, serial, width, height);
    }

    private final class Analytics implements AnalyticsListener {
        @Override
        public void onVideoInputFormatChanged(AnalyticsListener.EventTime time, Format format,
                                              DecoderReuseEvaluation evaluation) {
            if (loaded && !failed && readyOnce) checkVariant();
        }

        @Override
        public void onAudioInputFormatChanged(AnalyticsListener.EventTime time, Format format,
                                              DecoderReuseEvaluation evaluation) {
            if (loaded && !failed && readyOnce) checkVariant();
        }
    }

    private void classify(PlaybackException error) {
        int code = error.errorCode;
        String message = error.getMessage();
        Throwable cause = error.getCause();
        int status = 0;
        for (Throwable t = error; t != null; t = t.getCause()) {
            if (t instanceof HttpDataSource.InvalidResponseCodeException) {
                status = ((HttpDataSource.InvalidResponseCodeException) t).responseCode;
                break;
            }
        }
        if (cause != null && cause.getMessage() != null) message = message + ": " + cause.getMessage();
        int kind;
        // A read past the end of the resource (a 416 to a range request) is the
        // container pointing outside its own file: corrupt media, not a network
        // failure -- ExoPlayer's MP4 extractor meets exactly that on garbage.
        // Media3 reports it either way: as a read out of range, or as the 416.
        if (code == PlaybackException.ERROR_CODE_IO_READ_POSITION_OUT_OF_RANGE || status == 416) {
            fail(ERR_MEDIA, 0, message);
            return;
        }
        switch (code) {
            case PlaybackException.ERROR_CODE_IO_BAD_HTTP_STATUS:
            case PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_FAILED:
            case PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_TIMEOUT:
            case PlaybackException.ERROR_CODE_IO_INVALID_HTTP_CONTENT_TYPE:
            case PlaybackException.ERROR_CODE_IO_CLEARTEXT_NOT_PERMITTED:
            case PlaybackException.ERROR_CODE_IO_FILE_NOT_FOUND:
            case PlaybackException.ERROR_CODE_IO_NO_PERMISSION:
            case PlaybackException.ERROR_CODE_IO_UNSPECIFIED:
            case PlaybackException.ERROR_CODE_TIMEOUT:
                kind = ERR_NETWORK;
                break;
            case PlaybackException.ERROR_CODE_DRM_SCHEME_UNSUPPORTED:
            case PlaybackException.ERROR_CODE_DRM_PROVISIONING_FAILED:
            case PlaybackException.ERROR_CODE_DRM_DEVICE_REVOKED:
            case PlaybackException.ERROR_CODE_DRM_DISALLOWED_OPERATION:
                kind = ERR_KEY_SYSTEM;
                break;
            case PlaybackException.ERROR_CODE_DRM_LICENSE_ACQUISITION_FAILED:
            case PlaybackException.ERROR_CODE_DRM_LICENSE_EXPIRED:
            case PlaybackException.ERROR_CODE_DRM_SYSTEM_ERROR:
            case PlaybackException.ERROR_CODE_DRM_CONTENT_ERROR:
            case PlaybackException.ERROR_CODE_DRM_UNSPECIFIED:
                kind = ERR_LICENCE;
                break;
            default:
                kind = ERR_MEDIA;
                break;
        }
        fail(kind, status, message);
    }

    // ---- the plane (UI thread) ---------------------------------------------------------------------

    private void placePlane(double x, double y, double width, double height, boolean visible, int dw, int dh) {
        planeX = x;
        planeY = y;
        planeW = width;
        planeH = height;
        drawableW = dw;
        drawableH = dh;
        ViewGroup layout = VideoLayer.layout();
        if (stopped || layout == null) return;
        if (visible && view == null) {
            view = new SurfaceView(layout.getContext());
            view.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override
                public void surfaceCreated(SurfaceHolder holder) {
                    final Surface surface = holder.getSurface();
                    post(() -> {
                        viewSurface = surface;
                        if (player != null) player.setVideoSurface(surface);
                    });
                }

                @Override
                public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {}

                @Override
                public void surfaceDestroyed(SurfaceHolder holder) {
                    // The surface is gone when this returns, so the decoder has to
                    // have let go of it first: wait (bounded) for the player thread,
                    // which never waits on this one.
                    final Surface surface = holder.getSurface();
                    final Object done = new Object();
                    final boolean[] finished = {false};
                    boolean posted = handler.post(() -> {
                        if (viewSurface == surface) {
                            viewSurface = null;
                            if (player != null) useHeadlessSurface();
                        }
                        synchronized (done) {
                            finished[0] = true;
                            done.notifyAll();
                        }
                    });
                    if (!posted) return;
                    synchronized (done) {
                        long deadline = System.currentTimeMillis() + 2500;
                        while (!finished[0]) {
                            long left = deadline - System.currentTimeMillis();
                            if (left <= 0) break;
                            try {
                                done.wait(left);
                            } catch (InterruptedException e) {
                                Thread.currentThread().interrupt();
                                break;
                            }
                        }
                    }
                }
            });
            // Index 0: first in SDL's layout. The z-order is the media layer's
            // (VideoLayer), not the view order, but beneath is where it belongs.
            layout.addView(view, 0, new RelativeLayout.LayoutParams(1, 1));
        }
        if (view == null) return;
        if (visible != planeVisible) {
            planeVisible = visible;
            view.setVisibility(visible ? android.view.View.VISIBLE : android.view.View.GONE);
        }
        layoutPlane();
    }

    /**
     * The plane in the layout's pixels: the drawable is a fixed size scaled into
     * the window, centred (media::planeInWindow), and the picture is fitted inside
     * the element's box, aspect kept (object-fit: contain, media::fitContain).
     */
    private void layoutPlane() {
        if (view == null || stopped) return;
        ViewGroup layout = VideoLayer.layout();
        if (layout == null) return;
        double windowW = layout.getWidth(), windowH = layout.getHeight();
        double x = planeX, y = planeY, w = planeW, h = planeH;
        if (drawableW > 0 && drawableH > 0 && windowW > 0 && windowH > 0
                && (drawableW != windowW || drawableH != windowH)) {
            double scale = Math.min(windowW / drawableW, windowH / drawableH);
            double offsetX = (windowW - drawableW * scale) / 2;
            double offsetY = (windowH - drawableH * scale) / 2;
            x = offsetX + x * scale;
            y = offsetY + y * scale;
            w *= scale;
            h *= scale;
        }
        double pictureW = videoW * pixelRatio, pictureH = videoH;
        if (pictureW > 0 && pictureH > 0 && w > 0 && h > 0) {
            double scale = Math.min(w / pictureW, h / pictureH);
            double fitW = pictureW * scale, fitH = pictureH * scale;
            x += (w - fitW) / 2;
            y += (h - fitH) / 2;
            w = fitW;
            h = fitH;
        }
        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
                Math.max(1, (int) Math.round(w)), Math.max(1, (int) Math.round(h)));
        params.leftMargin = (int) Math.round(x);
        params.topMargin = (int) Math.round(y);
        view.setLayoutParams(params);
    }

    private void removePlane() {
        if (view == null) return;
        ViewGroup parent = (ViewGroup) view.getParent();
        if (parent != null) parent.removeView(view);
        view = null;
    }

    // ---- bytes across JNI: standard UTF-8 both ways, never modified UTF-8 --------------------------

    private static String text(byte[] utf8) {
        return utf8 == null ? "" : new String(utf8, StandardCharsets.UTF_8);
    }

    private static byte[] bytes(String text) {
        return text == null ? new byte[0] : text.getBytes(StandardCharsets.UTF_8);
    }

    private static final class Load {
        int serial;
        String url;
        double startTime;
        String mimeType;
        String keySystem;
        String licenceServer;
        byte[] clearKeysJson;
        byte[] serverCertificate;
        String audioLanguage;
        String textLanguage;
        double[] abr;
    }

    /** base64url without padding, as a JWK wants key ids and keys. For MediaPlayerAndroid's tests. */
    static String base64Url(byte[] data) {
        return Base64.encodeToString(data, Base64.NO_PADDING | Base64.NO_WRAP | Base64.URL_SAFE);
    }
}
