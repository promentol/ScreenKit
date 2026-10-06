// Copyright (c) ScreenKit contributors. MIT.
package dev.screenkit.net;

import android.util.Log;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.ConnectException;
import java.net.NoRouteToHostException;
import java.net.PortUnreachableException;
import java.net.ProtocolException;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.security.KeyStore;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;
import java.util.zip.ZipException;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLPeerUnverifiedException;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509ExtendedTrustManager;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.Cookie;
import okhttp3.CookieJar;
import okhttp3.Dispatcher;
import okhttp3.Headers;
import okhttp3.HttpUrl;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.ResponseBody;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.BufferedSink;
import okio.BufferedSource;
import okio.ByteString;
import okio.GzipSource;
import okio.InflaterSource;
import okio.Okio;

/**
 * One runtime's HTTP client: OkHttp, driven from native code by handle.
 *
 * <p>This is the whole Android half of {@code runtime/core/src/net/NetService.h},
 * and the counterpart of {@code NetServiceApple.mm}'s NSURLSession. There is no
 * protocol code here or anywhere above it any more: OkHttp resolves names, pools
 * connections, speaks HTTP/1.1 and HTTP/2, follows redirects, verifies
 * certificates against the platform trust store, and cookies live in
 * {@code android.webkit.CookieManager} -- the same shape React Native's
 * {@code NetworkingModule} has.
 *
 * <p>The one thing taken back from OkHttp is {@code Content-Encoding}. Left to
 * itself it offers {@code gzip} alone, so a {@code deflate} body reached the
 * page still compressed; it refuses an empty body labelled gzip that every
 * browser reads as empty; and it strips the headers off what it did decode. So
 * this client asks for {@code gzip, deflate} itself -- which turns OkHttp's
 * transparent gzip off -- and decodes with okio's own {@link GzipSource} and
 * {@link InflaterSource} ({@link #decoded}). The same answers NSURLSession and
 * the Linux client give.
 *
 * <p><b>Threads.</b> Every method here is called from the runtime's serial I/O
 * queue (native), and every callback out runs on an OkHttp dispatcher thread or
 * on the thread that reads a response body. None of them touches a C++ sink: they
 * call the {@code native*} methods below, which do nothing but copy the bytes and
 * re-post the event onto that same serial queue. A callback for an id the native
 * side has already forgotten is dropped there.
 *
 * <p><b>Backpressure is real.</b> OkHttp is pull-based, so a response body is
 * read by {@link Call#execute()}'s reader loop below and that loop simply stops
 * pulling once {@code flowWindow} bytes are delivered and unacknowledged. The
 * socket's receive buffer then fills and TCP holds the server back. Nothing is
 * buffered here.
 */
public final class HttpClient {
    private static final String TAG = "ScreenKit";
    private static final int READ_BUFFER = 64 * 1024;
    /** What Apple's client is given, and what the portable stack used before it. */
    private static final int CONNECT_TIMEOUT_MS = 30_000;

    // ---- the wire format shared with C++ -------------------------------------
    //
    // These numbers are the other half of `NetError` and `RedirectMode`
    // (runtime/core/src/net/NetError.h, NetService.h), which nothing on this side
    // of JNI can see. Both ends pin their values, the C++ end asserts the count
    // where it translates them (`toNetError`), and the block below fails class
    // loading if this end is edited into disagreeing with itself -- so a new kind
    // cannot silently remap every error the client reports.

    // Redirect modes, as HttpRequestSpec::RedirectMode orders them.
    private static final int REDIRECT_FOLLOW = 0;
    private static final int REDIRECT_MANUAL = 1;
    private static final int REDIRECT_ERROR = 2;
    private static final int REDIRECT_COUNT = 3;

    // NetError, as NetError.h orders it. ERR_COUNT is NetError.h's kNetErrorCount.
    private static final int ERR_NONE = 0;
    private static final int ERR_DNS = 1;
    private static final int ERR_CONNECT = 2;
    private static final int ERR_TLS = 3;
    private static final int ERR_NETWORK = 4;
    private static final int ERR_PROTOCOL = 5;
    private static final int ERR_REDIRECT = 6;
    private static final int ERR_DECODE = 7;
    private static final int ERR_URL = 8;
    private static final int ERR_UNSUPPORTED = 9;
    private static final int ERR_COUNT = 10;

    static {
        final int[] kinds = {ERR_NONE, ERR_DNS, ERR_CONNECT, ERR_TLS, ERR_NETWORK, ERR_PROTOCOL,
                             ERR_REDIRECT, ERR_DECODE, ERR_URL, ERR_UNSUPPORTED};
        if (kinds.length != ERR_COUNT) {
            throw new AssertionError("ERR_COUNT must match NetError.h's kNetErrorCount");
        }
        for (int i = 0; i < kinds.length; i++) {
            if (kinds[i] != i) throw new AssertionError("NetError's numbering is a wire format");
        }
        final int[] modes = {REDIRECT_FOLLOW, REDIRECT_MANUAL, REDIRECT_ERROR};
        if (modes.length != REDIRECT_COUNT) throw new AssertionError("REDIRECT_COUNT is wrong");
        for (int i = 0; i < modes.length; i++) {
            if (modes[i] != i) throw new AssertionError("RedirectMode is a wire format");
        }
    }

    /** Browsers open at most six HTTP/1.1 connections to one origin, and so did
     *  the portable stack's pool. OkHttp's own default is five. */
    private static final int MAX_PER_ORIGIN = 6;

    private final OkHttpClient client;
    private final ScreenKitCookieJar cookies;
    private final String userAgent;
    /** One thread for the WebSocket send-queue poll; idle sockets cost nothing. */
    private final ScheduledExecutorService ticks =
            Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(runnable, "screenkit-ws-drain");
                thread.setDaemon(true);
                return thread;
            });
    private final Map<Long, Exchange> requests = new ConcurrentHashMap<>();
    private final Map<Long, Socket2> sockets = new ConcurrentHashMap<>();
    private volatile boolean stopped;

    /**
     * @param testAnchors extra DER trust anchors for the test fixture's CA, or an
     *                    empty array. Never reachable from JS: it comes from
     *                    {@code RuntimeConfig::testTlsAnchors}.
     */
    public HttpClient(byte[][] testAnchors, String userAgent) throws IOException {
        this.userAgent = userAgent;
        this.cookies = new ScreenKitCookieJar();
        Dispatcher dispatcher = new Dispatcher();
        dispatcher.setMaxRequestsPerHost(MAX_PER_ORIGIN);
        OkHttpClient.Builder builder = new OkHttpClient.Builder()
                .dispatcher(dispatcher)
                .cookieJar(cookies)
                .connectTimeout(CONNECT_TIMEOUT_MS, TimeUnit.MILLISECONDS)
                // The web has no default read timeout -- `AbortSignal.timeout` and
                // `xhr.timeout` are its answer, and both work -- and an EventSource
                // or a WebSocket is meant to sit idle. A dead peer is TCP
                // keepalive's job, which OkHttp turns on with its own pings.
                .readTimeout(0, TimeUnit.MILLISECONDS)
                .writeTimeout(0, TimeUnit.MILLISECONDS)
                .callTimeout(0, TimeUnit.MILLISECONDS)
                .pingInterval(0, TimeUnit.MILLISECONDS);
        if (testAnchors != null && testAnchors.length > 0) {
            X509ExtendedTrustManager trust =
                    new AddedAnchors(defaultTrustManager(null), defaultTrustManager(anchorStore(testAnchors)));
            builder.sslSocketFactory(anchoredFactory(trust), trust);
        }
        this.client = builder.build();
    }

    // ---- requests -------------------------------------------------------------------

    public void startRequest(long id, String url, String method, String[] headerNames,
                             String[] headerValues, byte[] body, boolean streamingBody,
                             int redirectMode, boolean useCookies, boolean reportUpload,
                             long flowWindow) {
        if (stopped) {
            // Returning silently would never retire the call and its sink would
            // hear nothing at all; the native side only drops what it is told
            // about.
            nativeError(id, ERR_UNSUPPORTED, "the HTTP client has shut down");
            return;
        }
        HttpUrl parsed = HttpUrl.parse(url);
        if (parsed == null) {
            nativeError(id, ERR_URL, "not a URL this client can reach: " + url);
            return;
        }
        Exchange exchange = new Exchange(id, flowWindow, reportUpload, redirectMode);
        Request.Builder request = new Request.Builder().url(parsed);
        Headers.Builder headers = new Headers.Builder();
        if (headerNames != null) {
            for (int i = 0; i < headerNames.length; i++) {
                headers.add(headerNames[i], headerValues[i]);
            }
        }
        if (headers.get("User-Agent") == null) headers.add("User-Agent", userAgent);
        // Asked for here rather than left to OkHttp, which would offer gzip alone
        // and decode it itself -- see the class comment and `decoded`. Not with a
        // Range, as OkHttp does not: the byte offsets are of the encoded body.
        if (headers.get("Accept-Encoding") == null && headers.get("Range") == null) {
            headers.add("Accept-Encoding", "gzip, deflate");
        }
        request.headers(headers.build());

        RequestBody payload = null;
        if (streamingBody) {
            exchange.upload = new UploadBody(exchange);
            payload = exchange.upload;
        } else if (body != null && body.length > 0) {
            payload = new FixedBody(exchange, body);
        } else if (requiresBody(method)) {
            payload = RequestBody.create(new byte[0], null);
        }
        request.method(method, payload);

        OkHttpClient perRequest = client;
        if (redirectMode != REDIRECT_FOLLOW || !useCookies) {
            OkHttpClient.Builder builder = client.newBuilder();
            if (redirectMode != REDIRECT_FOLLOW) builder.followRedirects(false).followSslRedirects(false);
            // `credentials: 'omit'` sends none and stores none.
            if (!useCookies) builder.cookieJar(CookieJar.NO_COOKIES);
            perRequest = builder.build();
        }

        Call call = perRequest.newCall(request.build());
        exchange.call = call;
        requests.put(id, exchange);
        call.enqueue(new Callback() {
            @Override
            public void onFailure(Call failed, IOException e) {
                if (exchange.retire()) return;
                requests.remove(id);
                nativeError(id, classify(e, exchange.headDelivered), describe(e));
            }

            @Override
            public void onResponse(Call ok, Response response) {
                exchange.deliver(response);
            }
        });
    }

    public void appendRequestBody(long id, byte[] chunk) {
        Exchange exchange = requests.get(id);
        if (exchange == null || exchange.upload == null) return;
        exchange.upload.append(chunk);
    }

    public void finishRequestBody(long id) {
        Exchange exchange = requests.get(id);
        if (exchange == null || exchange.upload == null) return;
        exchange.upload.finish();
    }

    public void abortRequest(long id) {
        Exchange exchange = requests.remove(id);
        if (exchange == null) return;
        exchange.cancel();
    }

    public void acknowledgeResponseData(long id, long bytes) {
        Exchange exchange = requests.get(id);
        if (exchange == null) return;
        exchange.acknowledge(bytes);
    }

    // ---- sockets --------------------------------------------------------------------

    public void openSocket(long id, String url, String[] protocols) {
        if (stopped) {
            nativeSocketError(id, ERR_UNSUPPORTED, "the HTTP client has shut down");
            nativeSocketClose(id, 1006, "", false);
            return;
        }
        HttpUrl parsed = HttpUrl.parse(url.replaceFirst("^ws", "http"));
        if (parsed == null) {
            nativeSocketError(id, ERR_URL, "not a URL this client can reach: " + url);
            nativeSocketClose(id, 1006, "", false);
            return;
        }
        Request.Builder request = new Request.Builder().url(parsed).header("User-Agent", userAgent);
        if (protocols != null && protocols.length > 0) {
            StringBuilder joined = new StringBuilder();
            for (String protocol : protocols) {
                if (joined.length() > 0) joined.append(", ");
                joined.append(protocol);
            }
            request.header("Sec-WebSocket-Protocol", joined.toString());
        }
        Socket2 socket = new Socket2(id, protocols);
        sockets.put(id, socket);
        socket.socket = client.newWebSocket(request.build(), socket);
    }

    public void sendSocket(long id, boolean text, byte[] payload) {
        Socket2 socket = sockets.get(id);
        if (socket == null) return;
        socket.send(text, payload);
    }

    public void closeSocket(long id, int code, String reason) {
        Socket2 socket = sockets.get(id);
        if (socket == null) return;
        socket.closeGracefully(code, reason);
    }

    // ---- cookies --------------------------------------------------------------------

    /** The JS view: HttpOnly cookies are never in it. */
    public String cookiesFor(String url) {
        HttpUrl parsed = HttpUrl.parse(url);
        if (parsed == null) return "";
        return cookies.headerForJs(parsed);
    }

    /** The JS API may set neither an HttpOnly cookie nor one that shadows one. */
    public boolean setCookie(String url, String setCookie) {
        HttpUrl parsed = HttpUrl.parse(url);
        if (parsed == null) return false;
        return cookies.setFromJs(parsed, setCookie);
    }

    // ---- teardown -------------------------------------------------------------------

    /** Synchronous and idempotent: nothing is delivered after it returns. */
    public void shutdown() {
        stopped = true;
        for (Long id : new ArrayList<>(requests.keySet())) {
            Exchange exchange = requests.remove(id);
            if (exchange != null) exchange.cancel();
        }
        for (Long id : new ArrayList<>(sockets.keySet())) {
            Socket2 socket = sockets.remove(id);
            if (socket != null) socket.cancel();
        }
        client.dispatcher().cancelAll();
        client.connectionPool().evictAll();
        ticks.shutdownNow();
    }

    // ---- one request ------------------------------------------------------------------

    private final class Exchange {
        final long id;
        final long flowWindow;
        final boolean reportUpload;
        final int redirectMode;
        volatile Call call;
        volatile UploadBody upload;
        volatile boolean headDelivered;
        volatile boolean done;
        /** Delivered to the sink and not yet acknowledged. */
        private long unacknowledged;
        private final Object window = new Object();

        Exchange(long id, long flowWindow, boolean reportUpload, int redirectMode) {
            this.id = id;
            this.flowWindow = flowWindow;
            this.reportUpload = reportUpload;
            this.redirectMode = redirectMode;
        }

        /** True when the request is already finished or cancelled: say nothing more. */
        boolean retire() {
            if (done) return true;
            done = true;
            return false;
        }

        void cancel() {
            done = true;
            UploadBody body = upload;
            if (body != null) body.abort();
            synchronized (window) {
                window.notifyAll();
            }
            Call live = call;
            if (live != null) live.cancel();
        }

        void acknowledge(long bytes) {
            synchronized (window) {
                unacknowledged = bytes >= unacknowledged ? 0 : unacknowledged - bytes;
                window.notifyAll();
            }
        }

        /** An OkHttp dispatcher thread: the body is read here, so it may block. */
        void deliver(Response response) {
            try (Response held = response) {
                if (done) return;
                final int status = held.code();
                // A Location too: a 3xx without one is not a redirect, and both
                // fetch and the Apple client deliver it as an ordinary response.
                if (redirectMode == REDIRECT_ERROR && isRedirect(status)
                        && held.header("Location") != null) {
                    if (retire()) return;
                    requests.remove(id);
                    nativeError(id, ERR_REDIRECT,
                            "the response was a redirect and redirect: 'error' was asked for");
                    return;
                }
                if (hasConflictingLengths(held)) {
                    // RFC 9112 6.3: a message whose Content-Length lines disagree
                    // is unrecoverable. OkHttp reads the first and hands over that
                    // many bytes, which is a body the page cannot know was cut;
                    // NSURLSession, the Linux client and every browser refuse it.
                    if (retire()) return;
                    requests.remove(id);
                    nativeError(id, ERR_PROTOCOL, "the response has Content-Length values that disagree");
                    return;
                }
                Headers headers = held.headers();
                String[] names = new String[headers.size()];
                String[] values = new String[headers.size()];
                for (int i = 0; i < headers.size(); i++) {
                    names[i] = headers.name(i).toLowerCase(java.util.Locale.US);
                    values[i] = headers.value(i);
                }
                headDelivered = true;
                nativeHead(id, status, held.message(), names, values,
                        held.request().url().toString(), held.priorResponse() != null);
                ResponseBody body = held.body();
                if (body != null) {
                    // Closed here, not left to the response: a decoder owns an
                    // `Inflater`, whose native memory is only returned by `end()`.
                    try (BufferedSource source = decoded(body.source(), held.header("Content-Encoding"))) {
                        pump(source);
                    }
                }
                if (done) return;
                if (retire()) return;
                requests.remove(id);
                nativeEnd(id);
            } catch (IOException e) {
                if (retire()) return;
                requests.remove(id);
                nativeError(id, classify(e, headDelivered), describe(e));
            } catch (Throwable e) {
                // Throwable, not Exception: an OutOfMemoryError out of
                // `Arrays.copyOf` would otherwise leave the exchange in the map
                // and the request would never settle.
                if (retire()) return;
                requests.remove(id);
                nativeError(id, ERR_NETWORK, describe(e));
            }
        }

        /**
         * The reader loop, and the whole of backpressure: it stops pulling from the
         * source once `flowWindow` bytes are out and unacknowledged, and OkHttp
         * therefore stops reading from the socket.
         */
        private void pump(BufferedSource source) throws IOException {
            final byte[] buffer = new byte[READ_BUFFER];
            while (!done) {
                if (flowWindow > 0) {
                    synchronized (window) {
                        while (!done && unacknowledged >= flowWindow) {
                            try {
                                window.wait(1000);
                            } catch (InterruptedException interrupted) {
                                Thread.currentThread().interrupt();
                                throw new InterruptedIOException("the reader was interrupted");
                            }
                        }
                    }
                    if (done) return;
                }
                final int read = source.read(buffer, 0, buffer.length);
                if (read <= 0) return;
                if (done) return;
                // Counted *before* the chunk goes out, and under the same lock the
                // waiter uses. Counting afterwards loses an acknowledgement that
                // lands in between -- it clamps to zero against a count that is
                // still zero -- and the reader then waits for bytes the page has
                // already read. With a window at or under one read that is a hang.
                if (flowWindow > 0) {
                    synchronized (window) {
                        unacknowledged += read;
                    }
                }
                nativeData(id, Arrays.copyOf(buffer, read));
            }
        }

        void uploaded(long sent, long total, boolean complete) {
            if (!reportUpload || done) return;
            nativeUpload(id, sent, total, complete);
        }
    }

    /** A complete body, with upload progress reported as OkHttp writes it. */
    private final class FixedBody extends RequestBody {
        private final Exchange exchange;
        private final byte[] bytes;

        FixedBody(Exchange exchange, byte[] bytes) {
            this.exchange = exchange;
            this.bytes = bytes;
        }

        @Override
        public MediaType contentType() {
            // Null on purpose, and this is the whole reason the type is set as a
            // header instead: OkHttp writes a non-null body type over whatever the
            // request carried, and a body whose type the page did not give -- an
            // ArrayBuffer, a typeless Blob -- has no Content-Type in a browser.
            return null;
        }

        @Override
        public long contentLength() {
            return bytes.length;
        }

        @Override
        public void writeTo(BufferedSink sink) throws IOException {
            int written = 0;
            while (written < bytes.length) {
                final int step = Math.min(64 * 1024, bytes.length - written);
                sink.write(bytes, written, step);
                written += step;
                sink.flush();
                exchange.uploaded(written, bytes.length, written == bytes.length);
            }
            if (bytes.length == 0) exchange.uploaded(0, 0, true);
        }
    }

    /**
     * A `ReadableStream` request body: the chunks arrive from JS one at a time and
     * this hands them to OkHttp as they come, chunked because the length is
     * unknown. {@link #abort} makes the write fail rather than end, so an aborted
     * upload never looks to the server like a body that finished.
     */
    private final class UploadBody extends RequestBody {
        private final Exchange exchange;
        private final ArrayDeque<byte[]> pending = new ArrayDeque<>();
        private boolean ended;
        private boolean aborted;
        private long sent;

        UploadBody(Exchange exchange) {
            this.exchange = exchange;
        }

        @Override
        public MediaType contentType() {
            return null;  // see FixedBody.contentType
        }

        @Override
        public long contentLength() {
            return -1;  // chunked
        }

        @Override
        public boolean isOneShot() {
            // The chunks are gone once written. Without this OkHttp may call
            // writeTo again on a retry, and the server would see a body that
            // ends cleanly with nothing in it.
            return true;
        }

        void append(byte[] chunk) {
            synchronized (pending) {
                if (ended || aborted) return;
                pending.addLast(chunk);
                pending.notifyAll();
            }
        }

        void finish() {
            synchronized (pending) {
                ended = true;
                pending.notifyAll();
            }
        }

        void abort() {
            synchronized (pending) {
                aborted = true;
                pending.clear();
                pending.notifyAll();
            }
        }

        @Override
        public void writeTo(BufferedSink sink) throws IOException {
            for (;;) {
                byte[] chunk;
                synchronized (pending) {
                    while (pending.isEmpty() && !ended && !aborted) {
                        try {
                            pending.wait(1000);
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            throw new InterruptedIOException("the upload was interrupted");
                        }
                    }
                    if (aborted) throw new IOException("the request body stream was aborted");
                    if (pending.isEmpty() && ended) break;
                    chunk = pending.removeFirst();
                }
                sink.write(chunk);
                sink.flush();
                sent += chunk.length;
                exchange.uploaded(sent, -1, false);
            }
            exchange.uploaded(sent, -1, true);
        }
    }

    // ---- one socket -------------------------------------------------------------------

    /** Named for what it is rather than shadowing {@link java.net.Socket}. */
    private final class Socket2 extends WebSocketListener {
        final long id;
        /** What the handshake offered, which is all the server may pick from. */
        final List<String> offered;
        volatile WebSocket socket;
        volatile boolean opened;
        volatile boolean done;
        volatile boolean closeSent;
        /** Payload bytes handed to OkHttp and not yet on the wire, oldest first. */
        private final ArrayDeque<Long> queued = new ArrayDeque<>();
        private long queuedBytes;

        Socket2(long id, String[] protocols) {
            this.id = id;
            this.offered = protocols == null ? Collections.<String>emptyList() : Arrays.asList(protocols);
        }

        void send(boolean text, byte[] payload) {
            WebSocket live = socket;
            if (live == null || done) return;
            final boolean accepted = text
                    ? live.send(new String(payload, java.nio.charset.StandardCharsets.UTF_8))
                    : live.send(ByteString.of(payload));
            if (!accepted) {
                // The socket is closing or closed: nothing will be sent, and
                // `bufferedAmount` must not wait for it for ever.
                nativeSocketSent(id, payload.length);
                return;
            }
            synchronized (queued) {
                queued.addLast((long) payload.length);
                queuedBytes += payload.length;
            }
            drainSent();
        }

        /**
         * `bufferedAmount` goes down as OkHttp puts the bytes on the wire, which
         * `queueSize()` is how it reports. Polled, because OkHttp has no
         * per-message completion callback; the poll stops as soon as the queue is
         * empty, so an idle socket costs nothing.
         */
        private void drainSent() {
            WebSocket live = socket;
            if (live == null) return;
            final long outstanding = live.queueSize();
            final List<Long> sent = new ArrayList<>();
            synchronized (queued) {
                while (!queued.isEmpty() && queuedBytes - queued.peekFirst() >= outstanding) {
                    final long bytes = queued.removeFirst();
                    queuedBytes -= bytes;
                    sent.add(bytes);
                }
            }
            for (Long bytes : sent) nativeSocketSent(id, bytes);
            boolean more;
            synchronized (queued) {
                more = !queued.isEmpty();
            }
            if (more && !done && !stopped) {
                try {
                    ticks.schedule(this::drainSent, 5, TimeUnit.MILLISECONDS);
                } catch (RuntimeException stopping) {
                    // The client is shutting down; bufferedAmount goes with it.
                }
            }
        }

        void closeGracefully(int code, String reason) {
            WebSocket live = socket;
            if (live == null || closeSent) return;
            closeSent = true;
            try {
                // OkHttp refuses a close with no status code, so a bare close()
                // goes out as 1000 -- the same recorded divergence NSURLSession
                // forces.
                live.close(code == 0 ? 1000 : code,
                        reason == null || reason.isEmpty() ? null : reason);
            } catch (IllegalArgumentException refused) {
                // A reason over 123 UTF-8 bytes, or a code OkHttp reserves.
                // Letting it out would leave the socket open and the page's
                // close() never answered, so the connection goes the hard way.
                Log.w(TAG, "close(" + code + ") refused: " + refused);
                cancel();
                nativeSocketError(id, ERR_PROTOCOL, describe(refused));
                nativeSocketClose(id, 1006, "", false);
            }
        }

        void cancel() {
            done = true;
            WebSocket live = socket;
            socket = null;
            flushQueued();
            if (live != null) live.cancel();
        }

        /// Whatever is still queued will never be sent. Reporting it keeps
        /// `bufferedAmount` from standing at a number that can no longer fall,
        /// which a page waiting for it to reach zero would hang on.
        private void flushQueued() {
            final List<Long> abandoned = new ArrayList<>();
            synchronized (queued) {
                while (!queued.isEmpty()) {
                    final long bytes = queued.removeFirst();
                    queuedBytes -= bytes;
                    abandoned.add(bytes);
                }
            }
            for (Long bytes : abandoned) nativeSocketSent(id, bytes);
        }

        private boolean retire() {
            if (done) return true;
            done = true;
            sockets.remove(id);
            flushQueued();
            return false;
        }

        @Override
        public void onOpen(WebSocket ws, Response response) {
            if (done) return;
            final String protocol = response.header("Sec-WebSocket-Protocol");
            final String extensions = response.header("Sec-WebSocket-Extensions");
            final String refused = refusal(protocol, extensions);
            if (refused != null) {
                // RFC 6455 4.1: "Fail the WebSocket Connection". OkHttp checks the
                // accept key and nothing else here -- a subprotocol nobody offered
                // opens the socket, and an extension it does not know opens it
                // and then closes it with 1010 -- so the page would see `open`
                // for a handshake a browser refuses. It never does.
                if (retire()) return;
                ws.cancel();
                nativeSocketError(id, ERR_PROTOCOL, refused);
                nativeSocketClose(id, 1006, "", false);
                return;
            }
            opened = true;
            nativeSocketOpen(id, protocol == null ? "" : protocol, extensions == null ? "" : extensions);
        }

        /** Why this 101 must be refused, or null. OkHttp offers `permessage-deflate`
         *  on every socket, so that is the one extension a server may answer with. */
        private String refusal(String protocol, String extensions) {
            if (protocol != null && !protocol.isEmpty() && !offered.contains(protocol)) {
                return "the server chose a subprotocol that was not offered: " + protocol;
            }
            if (extensions != null) {
                for (String extension : extensions.split(",")) {
                    final String name = extension.split(";", 2)[0].trim();
                    if (!name.isEmpty() && !"permessage-deflate".equalsIgnoreCase(name)) {
                        return "the server answered with an extension that was not offered: " + name;
                    }
                }
            }
            return null;
        }

        @Override
        public void onMessage(WebSocket ws, String text) {
            if (done) return;
            nativeSocketMessage(id, true, text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }

        @Override
        public void onMessage(WebSocket ws, ByteString bytes) {
            if (done) return;
            nativeSocketMessage(id, false, bytes.toByteArray());
        }

        @Override
        public void onClosing(WebSocket ws, int code, String reason) {
            // The peer started the closing handshake; answer it so the connection
            // ends cleanly, and let onClosed report it.
            if (done) return;
            if (!closeSent) {
                closeSent = true;
                ws.close(code == 1005 ? 1000 : code, null);
            }
        }

        @Override
        public void onClosed(WebSocket ws, int code, String reason) {
            if (retire()) return;
            nativeSocketClose(id, code, reason == null ? "" : reason, true);
        }

        @Override
        public void onFailure(WebSocket ws, Throwable t, Response response) {
            if (retire()) return;
            final int kind = t instanceof IOException ? classify((IOException) t, opened) : ERR_NETWORK;
            nativeSocketError(id, kind, describe(t));
            nativeSocketClose(id, 1006, "", false);
        }
    }

    // ---- cookies, in the platform store --------------------------------------------------

    /**
     * {@code android.webkit.CookieManager}: the store Chrome and every WebView on
     * the device share, and what React Native's {@code ForwardingCookieHandler}
     * uses. Its rules -- the public-suffix list, the eviction policy, how long a
     * session cookie lives -- are the platform's, and differ from Apple's
     * {@code NSHTTPCookieStorage} (spec-platform-http-clients.md).
     *
     * <p>A device image with no WebView has no such store; the client then keeps
     * cookies in memory for the life of the runtime rather than failing every
     * request, and says so once in the log.
     */
    private static final class ScreenKitCookieJar implements CookieJar {
        private final android.webkit.CookieManager platform;
        private final java.net.CookieManager fallback;
        /**
         * Every cookie this runtime has stored **without** {@code HttpOnly}, by
         * RFC 6265's identity for a cookie (name, domain, path, host-only), with
         * the value it was stored with.
         *
         * <p>The store hands back a bare {@code name=value} string with no
         * attributes, so it cannot say which of its cookies are HttpOnly -- and
         * "HttpOnly is never visible to JS" is older than this client
         * (runtime/js/README.md). So the JS view is an allow-list rather than a
         * block-list: a pair the store returns is shown only when a cookie this
         * runtime saw stored without the flag has that name and that value and
         * applies to the URL. Anything else -- an HttpOnly cookie, and any cookie
         * whose flags this runtime never saw, such as one a WebView in this app
         * or an earlier run left in the store -- stays hidden. That can hide too
         * much, never too little: an HttpOnly value only shows if the page could
         * already see the very same name and value.
         */
        private final Map<String, Cookie> visible = new ConcurrentHashMap<>();
        /** The same identities, for the ones seen stored with the flag. */
        private final java.util.Set<String> httpOnly =
                java.util.Collections.newSetFromMap(new ConcurrentHashMap<String, Boolean>());

        ScreenKitCookieJar() {
            android.webkit.CookieManager manager = null;
            try {
                manager = android.webkit.CookieManager.getInstance();
                manager.setAcceptCookie(true);
            } catch (Throwable noWebView) {
                Log.w(TAG, "no android.webkit.CookieManager on this device: cookies are per-runtime and"
                        + " in memory (" + noWebView + ")");
                manager = null;
            }
            platform = manager;
            fallback = platform == null ? new java.net.CookieManager() : null;
        }

        @Override
        public List<Cookie> loadForRequest(HttpUrl url) {
            final String header = rawHeader(url);
            if (header == null || header.isEmpty()) return Collections.emptyList();
            final List<Cookie> out = new ArrayList<>();
            for (String pair : header.split(";")) {
                final String trimmed = pair.trim();
                if (trimmed.isEmpty()) continue;
                final int equals = trimmed.indexOf('=');
                if (equals <= 0) continue;
                // Trimmed on both sides: the store hands back whatever was
                // written into it, and `Cookie.Builder` throws on a name or value
                // with space around the `=` -- which would fail every request to
                // that host, not just the cookie.
                final String name = trimmed.substring(0, equals).trim();
                final String value = trimmed.substring(equals + 1).trim();
                if (name.isEmpty()) continue;
                try {
                    out.add(new Cookie.Builder()
                            .name(name)
                            .value(value)
                            .domain(url.host())
                            .path("/")
                            .build());
                } catch (IllegalArgumentException refused) {
                    // One unusable cookie must not cost the request.
                    Log.w(TAG, "dropping a cookie the store returned: " + refused);
                }
            }
            return out;
        }

        @Override
        public void saveFromResponse(HttpUrl url, List<Cookie> incoming) {
            for (Cookie cookie : incoming) {
                remember(cookie);
                store(url, cookie.toString());
            }
        }

        private static String identity(Cookie cookie) {
            return cookie.name() + '\n' + cookie.domain() + '\n' + cookie.path() + '\n' + cookie.hostOnly();
        }

        /** Bookkeeping for the JS view, before the cookie goes into the store. */
        private void remember(Cookie cookie) {
            final String key = identity(cookie);
            if (cookie.httpOnly()) {
                visible.remove(key);
                httpOnly.add(key);
                return;
            }
            httpOnly.remove(key);
            if (cookie.expiresAt() <= System.currentTimeMillis()) {
                visible.remove(key);  // a deletion
            } else {
                visible.put(key, cookie);
            }
        }

        /** A cookie this runtime saw stored without HttpOnly has this name and value and applies here. */
        private boolean seenVisible(HttpUrl url, String name, String value, long now) {
            for (Cookie cookie : visible.values()) {
                if (cookie.name().equals(name) && cookie.value().equals(value)
                        && cookie.expiresAt() > now && cookie.matches(url)) {
                    return true;
                }
            }
            return false;
        }

        private String rawHeader(HttpUrl url) {
            if (platform != null) return platform.getCookie(url.toString());
            try {
                final Map<String, List<String>> got =
                        fallback.get(url.uri(), Collections.<String, List<String>>emptyMap());
                final List<String> values = got.get("Cookie");
                if (values == null || values.isEmpty()) return null;
                final StringBuilder joined = new StringBuilder();
                for (String value : values) {
                    if (joined.length() > 0) joined.append("; ");
                    joined.append(value);
                }
                return joined.toString();
            } catch (IOException e) {
                return null;
            }
        }

        private void store(HttpUrl url, String setCookie) {
            if (platform != null) {
                platform.setCookie(url.toString(), setCookie);
                return;
            }
            try {
                fallback.put(url.uri(),
                        Collections.singletonMap("Set-Cookie", Collections.singletonList(setCookie)));
            } catch (IOException ignored) {
                // An in-memory store that refuses a cookie has nowhere to report it.
            }
        }

        /** The JS view of the store: only what {@link #visible} vouches for. */
        String headerForJs(HttpUrl url) {
            final long now = System.currentTimeMillis();
            final StringBuilder out = new StringBuilder();
            for (Cookie cookie : loadForRequest(url)) {
                if (!seenVisible(url, cookie.name(), cookie.value(), now)) continue;
                if (out.length() > 0) out.append("; ");
                out.append(cookie.name()).append('=').append(cookie.value());
            }
            return out.toString();
        }

        /**
         * The JS API may not set an HttpOnly cookie, may not replace one (RFC
         * 6265 5.3 step 11), and -- because the store cannot say which of its
         * cookies are HttpOnly -- may not replace any cookie of the same name
         * that the JS view cannot see either: one this runtime never saw stored
         * could be exactly that.
         */
        boolean setFromJs(HttpUrl url, String setCookie) {
            final Cookie parsed = Cookie.parse(url, setCookie);
            if (parsed == null) return false;
            if (parsed.httpOnly()) return false;
            if (httpOnly.contains(identity(parsed))) return false;
            if (parsed.secure() && !url.isHttps()) return false;
            final long now = System.currentTimeMillis();
            for (Cookie stored : loadForRequest(url)) {
                if (stored.name().equals(parsed.name()) && !seenVisible(url, stored.name(), stored.value(), now)) {
                    return false;
                }
            }
            remember(parsed);
            store(url, setCookie);
            return true;
        }
    }

    // ---- TLS ---------------------------------------------------------------------------

    private static KeyStore anchorStore(byte[][] anchors) throws IOException {
        try {
            final CertificateFactory certificates = CertificateFactory.getInstance("X.509");
            final KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
            store.load(null, null);
            for (int i = 0; i < anchors.length; i++) {
                final X509Certificate certificate = (X509Certificate)
                        certificates.generateCertificate(new ByteArrayInputStream(anchors[i]));
                store.setCertificateEntry("screenkit-test-anchor-" + i, certificate);
            }
            return store;
        } catch (Exception e) {
            throw new IOException("cannot build a trust store for the test anchors: " + e, e);
        }
    }

    private static SSLSocketFactory anchoredFactory(X509ExtendedTrustManager trust) throws IOException {
        try {
            final SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, new TrustManager[] {trust}, null);
            return context.getSocketFactory();
        } catch (Exception e) {
            throw new IOException("cannot build an SSL context for the test anchors: " + e, e);
        }
    }

    /**
     * `store` null means the platform's own trust store. Extended, not plain: see
     * {@link AddedAnchors}, whose whole correctness rests on it.
     */
    private static X509ExtendedTrustManager defaultTrustManager(KeyStore store) throws IOException {
        try {
            final TrustManagerFactory factory =
                    TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            factory.init(store);
            for (TrustManager manager : factory.getTrustManagers()) {
                if (manager instanceof X509ExtendedTrustManager) {
                    return (X509ExtendedTrustManager) manager;
                }
            }
        } catch (Exception e) {
            throw new IOException("cannot build a trust manager: " + e, e);
        }
        throw new IOException("no X509ExtendedTrustManager from " + TrustManagerFactory.getDefaultAlgorithm());
    }

    /**
     * The system's trust store, plus the anchors the runtime was configured with --
     * never instead of it, and never a bypass: both managers do the full RFC 5280
     * check, and a chain that satisfies neither fails.
     *
     * <p>It extends {@link X509ExtendedTrustManager} and forwards the overloads that
     * carry the {@link Socket} or {@link SSLEngine}, and that is load-bearing rather
     * than tidy. Host-name verification is done by the platform's trust manager
     * inside those overloads; a custom manager that implements only plain
     * {@code X509TrustManager} is called through the two-argument form instead, and
     * the host name is then never checked at all. Written that way first, the
     * suite's wrong-host row resolved with status 200 on the device.
     */
    private static final class AddedAnchors extends X509ExtendedTrustManager {
        private final X509ExtendedTrustManager system;
        private final X509ExtendedTrustManager added;

        AddedAnchors(X509ExtendedTrustManager system, X509ExtendedTrustManager added) {
            this.system = system;
            this.added = added;
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType)
                throws CertificateException {
            system.checkClientTrusted(chain, authType);
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType, Socket socket)
                throws CertificateException {
            system.checkClientTrusted(chain, authType, socket);
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType, SSLEngine engine)
                throws CertificateException {
            system.checkClientTrusted(chain, authType, engine);
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType)
                throws CertificateException {
            try {
                system.checkServerTrusted(chain, authType);
            } catch (CertificateException notPublic) {
                added.checkServerTrusted(chain, authType);
            }
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType, Socket socket)
                throws CertificateException {
            try {
                system.checkServerTrusted(chain, authType, socket);
            } catch (CertificateException notPublic) {
                added.checkServerTrusted(chain, authType, socket);
            }
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType, SSLEngine engine)
                throws CertificateException {
            try {
                system.checkServerTrusted(chain, authType, engine);
            } catch (CertificateException notPublic) {
                added.checkServerTrusted(chain, authType, engine);
            }
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
            return system.getAcceptedIssuers();
        }
    }

    // ---- shared helpers -----------------------------------------------------------------

    private static boolean isRedirect(int status) {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    /**
     * The body as the page should see it. `gzip` and `deflate` are decoded; the
     * headers that describe the encoding are left as the server sent them, as a
     * browser leaves them. `deflate` is RFC 9110's zlib format, but servers send
     * raw DEFLATE under the same name often enough that every browser takes
     * both, so the first two bytes decide which. An empty body is an empty body
     * whatever it is labelled, which is where OkHttp's own gzip failed. A stream
     * that stops before its end fails the read -- `EOFException` from okio --
     * rather than handing over a short body that looks whole.
     */
    private static BufferedSource decoded(BufferedSource source, String encoding) throws IOException {
        if (encoding == null) return source;
        final String name = encoding.trim().toLowerCase(java.util.Locale.US);
        final boolean gzip = "gzip".equals(name) || "x-gzip".equals(name);
        if (!gzip && !"deflate".equals(name)) return source;  // one nobody asked for: as it came
        if (!source.request(1)) return source;
        if (gzip) return Okio.buffer(new GzipSource(source));
        boolean zlib = false;
        if (source.request(2)) {
            final int cmf = source.getBuffer().getByte(0) & 0xff;
            final int flg = source.getBuffer().getByte(1) & 0xff;
            zlib = (cmf & 0x0f) == 8 && ((cmf << 8) | flg) % 31 == 0;
        }
        return Okio.buffer(new InflaterSource(source, new Inflater(!zlib)));
    }

    private static boolean hasConflictingLengths(Response response) {
        String seen = null;
        for (String line : response.headers("Content-Length")) {
            for (String value : line.split(",")) {
                final String trimmed = value.trim();
                if (trimmed.isEmpty()) continue;
                if (seen == null) seen = trimmed;
                else if (!seen.equals(trimmed)) return true;
            }
        }
        return false;
    }

    /** RFC 9110: these methods must carry a body, even an empty one. */
    private static boolean requiresBody(String method) {
        return "POST".equals(method) || "PUT".equals(method) || "PATCH".equals(method)
                || "PROPPATCH".equals(method) || "REPORT".equals(method);
    }

    /**
     * OkHttp's exception hierarchy folded into {@code NetError}'s ten kinds.
     * `afterHead` separates a connection that never came up from one that broke
     * mid-exchange, as the Apple client's `classifyError` does.
     */
    private static int classify(IOException e, boolean afterHead) {
        if (e instanceof UnknownHostException) return ERR_DNS;
        if (e instanceof SSLHandshakeException || e instanceof SSLPeerUnverifiedException) return ERR_TLS;
        if (e.getCause() instanceof CertificateException) return ERR_TLS;
        if (e instanceof SSLException) return afterHead ? ERR_NETWORK : ERR_TLS;
        if (e instanceof ConnectException || e instanceof NoRouteToHostException
                || e instanceof PortUnreachableException) {
            return ERR_CONNECT;
        }
        if (e instanceof SocketTimeoutException) return afterHead ? ERR_NETWORK : ERR_CONNECT;
        if (e instanceof ProtocolException) {
            final String message = e.getMessage();
            // OkHttp's own words for running out of redirects.
            if (message != null && message.startsWith("Too many follow-up requests")) return ERR_REDIRECT;
            return ERR_PROTOCOL;
        }
        if (e instanceof ZipException || e.getCause() instanceof DataFormatException) return ERR_DECODE;
        return afterHead ? ERR_NETWORK : ERR_CONNECT;
    }

    private static String describe(Throwable e) {
        final String message = e.getMessage();
        if (message != null && !message.isEmpty()) return e.getClass().getSimpleName() + ": " + message;
        return e.getClass().getSimpleName();
    }

    // ---- into C++ (registered by JNI_OnLoad; see net/NetServiceAndroid.cpp) ---------------

    private static native void nativeHead(long id, int status, String statusText,
                                          String[] headerNames, String[] headerValues, String url,
                                          boolean redirected);

    private static native void nativeData(long id, byte[] chunk);

    private static native void nativeEnd(long id);

    private static native void nativeError(long id, int kind, String message);

    private static native void nativeUpload(long id, long sent, long total, boolean complete);

    private static native void nativeSocketOpen(long id, String protocol, String extensions);

    private static native void nativeSocketMessage(long id, boolean text, byte[] data);

    private static native void nativeSocketSent(long id, long bytes);

    private static native void nativeSocketError(long id, int kind, String message);

    private static native void nativeSocketClose(long id, int code, String reason, boolean wasClean);
}
