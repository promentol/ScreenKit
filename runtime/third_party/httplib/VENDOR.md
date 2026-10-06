# cpp-httplib, vendored

One MIT-licensed header, taken from one upstream commit, with **one small patch table applied on the
way in** (`tools/vendor/httplib.rules`), and pinned twice: the sha256 of upstream's file as fetched,
and the sha256 of what the patches make of it. It is the Linux HTTP client's only dependency:
`runtime/core/src/net/NetServiceLinux.cpp` is the whole of the translation from `NetService` onto
`httplib::Client`, and nothing else in the tree includes this.

| | |
|---|---|
| Upstream | https://github.com/yhirose/cpp-httplib |
| Version | `v0.56.0` |
| Commit | `278c2979e8c68468960c3073e28e1c51b098d6a4` |
| Released | 2026-09-11 |
| Licence | MIT, (c) 2017 yhirose -- see `LICENSE` |
| upstream `httplib.h` sha256 | `1f99e51881c4c9d0649b27c611442c2f4d9bcfec5a22a14d5fcd1f8106f730b4` |
| patched `httplib.h` sha256 (what is here) | `ed56be21ef4224cda0d84dc85205307fe748fc51edc063af02739a728ba0d0c1` |
| `LICENSE` sha256 | `4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b` |

## The patches

Three rows in `tools/vendor/httplib.rules`, all for one behaviour, and each must match exactly once
or the import stops. Upstream's `WebSocket::read` echoes a Close frame and returns `Fail` after
**discarding its payload** -- the peer's status code and reason -- and a transport failure returns
`Fail` too, so a client cannot tell a clean close from a dropped connection. The rows keep the
payload (`WebSocket::peer_close_payload_`) and expose it (`WebSocketClient::peer_close_payload()`),
and `NetServiceLinux.cpp` reports the peer's own code and reason as the other two clients do. Still
true of upstream master on 2026-09-19; the better home for it is upstream.

## Never hand-edit this tree

`runtime/cmake/HttplibVendored.cmake` re-checks the patched header's and the licence's sha256 at
configure time, so an edit here fails the Linux build rather than shipping. A change to the patch
table is a new `PATCHED_SHA256`: the script prints it, writes nothing, and asks for the diff to be
read first. To take a newer upstream:

```sh
sh tools/vendor/httplib.sh --version v0.57.0   # prints the new pin; writes nothing
# ...put the printed values in the pin block of tools/vendor/httplib.sh and in the table above
sh tools/vendor/httplib.sh                     # re-import at the new pin
sh tools/vendor/httplib.sh --check             # what the build does
```

## Why this library

Linux is the one target with no platform HTTP client to put behind `NetService`
(`spec-linux-http-client.md`): there is no NSURLSession and no OkHttp, and the alternative was
reviving the portable protocol stack that `spec-platform-http-clients.md` deleted. cpp-httplib is a
single header with no build system of its own, it speaks HTTP/1.1, and its TLS is the image's own
OpenSSL -- so nothing about TLS or the CA roots is bundled here, which is the rule that matters most.

## What it does not do

- **No HTTP/2.** Apple and Android get HTTP/2 from their platform clients; Linux is HTTP/1.1 only.
- **No WebSocket extensions**, and no check of what the server answered: a subprotocol the client
  never offered, or any `Sec-WebSocket-Extensions`, opens the socket. RFC 6455 4.1 says fail it, and
  `NetServiceLinux.cpp` does.
- **Its decoder cannot read raw DEFLATE**, which servers send as `deflate` often enough that every
  browser accepts it. `set_decompress(false)`, and `NetServiceLinux.cpp` decodes bodies itself.
- **`stop()` waits for a connect.** The client holds its socket mutex through the whole connect and
  TLS handshake, and `stop()` takes that mutex -- so it cannot interrupt either, and neither can
  anything else, since the descriptor is local to the connect until it finishes. `NetServiceLinux.cpp`
  keeps a duplicate of every socket it opens (`set_socket_options`) and shuts that down first.
- **Its name resolution cannot be interrupted** (`getaddrinfo` on the request's thread), so names are
  resolved in `NetServiceLinux.cpp` on a thread whose answer can be abandoned, and handed over with
  `set_hostname_addr_map`.
- **Synchronous.** A request blocks its thread until the response ends, which is why
  `NetServiceLinux.cpp` runs each in-flight request on a worker thread of its own and posts every
  sink call back onto the runtime's `IoQueue`. It is also what makes backpressure real: the reader
  callback simply stops returning, so the socket stops being drained.

## Build-time configuration

Set in `NetServiceLinux.cpp` before the include, not here:

| Macro | Set where | Why |
|---|---|---|
| `CPPHTTPLIB_OPENSSL_SUPPORT` | `runtime/CMakeLists.txt` | TLS from the image's OpenSSL; without it the build fails at configure time rather than falling back to plaintext |
| `CPPHTTPLIB_EXPECT_100_THRESHOLD 0` | `NetServiceLinux.cpp` | a browser never sends `Expect: 100-continue`; upstream's default adds it to any body over 1 KiB |

The first changes this header's **class layouts**, so it is set on the target rather than in the one
`.cpp`: a second translation unit that ever included the header without it would be an ODR violation
that links and then misbehaves. The second changes no layout and stays with its reason.
`CPPHTTPLIB_ZLIB_SUPPORT` is deliberately not set: bodies are decoded by the client, not the library.

## What a version bump has to be re-checked against

`NetServiceLinux.cpp` reaches past the public API in one place, and relies on two hooks doing what
their names say, all listed in its file comment: `Request::content_provider_` / `content_length_` /
`is_chunked_content_provider_` with `detail::ContentProviderAdapter` (every request body goes out
through a provider, because the public `Post` overloads write `text/plain` into any typeless body),
and `set_socket_options` / `set_hostname_addr_map` on both `Client` and `WebSocketClient`. The patch
table must also still apply: the import refuses a row that no longer matches exactly once. Check all
three when moving the pin.
