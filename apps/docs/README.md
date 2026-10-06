# @screenkit/docs

The documentation site: the developer guide and the environment reference, built with
[Astro Starlight](https://starlight.astro.build/).

```sh
npm install
npm run dev      # http://localhost:4321
npm run build    # -> dist/
npm run preview  # serve the built site
```

## Where things live

```
src/content/docs/
├── index.mdx              the landing page
├── start/                 what it is · quickstart · the build pipeline
├── guides/                canvases · video · iframes · CSS · input · networking
├── examples/              one page per app in the repo's examples/, with a screenshot
└── reference/             the environment contract · shaka · vite plugin · CLI · targets
```

`astro.config.mjs` holds the sidebar, and it is the only place the order of the guide is decided.
Adding a page means writing the file **and** adding it to the sidebar; a page not listed there is
still built and reachable, just not linked.

## Screenshots

`src/assets/examples/*.png` are frames the runtime captured of itself. To refresh one:

```sh
npm run build:screenkit -w screenkit-example-pixi-hello
SCREENKIT_CAPTURE=apps/docs/src/assets/examples/pixi-hello.png SCREENKIT_CAPTURE_DELAY_MS=3000 \
  runtime/build/macos/screenkit-host --window --size 1280x720 examples/pixi-hello/app.skpkg
```

`SCREENKIT_CAPTURE` reads the default framebuffer after the frame's GL work and before the present,
so what lands in the PNG is what the screen was about to show. Astro converts them to WebP at build
time; commit the PNG, not the WebP.

## The one build warning

`astro build` prints one line:

```
Could not render `/404` from route `/[...slug]` as it conflicts with higher priority route `/404`
```

That is expected. Starlight ships its own `/404` route and renders `src/content/docs/404.md` inside
it, so the custom page *is* used — the catch-all route simply defers to the dedicated one. The
`dist/404.html` it produces contains our content; check for the tagline if you ever doubt it.

## The one rule for this content

**Nothing here may describe an API the runtime does not have.** These pages were written from
`Architecture.md` and `runtime/js/README.md`, which are themselves written from the test suite, and
they inherit that contract: if a page claims a behaviour, some row asserts it. When the runtime
changes, the source documents change first and this site follows — it is not an independent account
of how ScreenKit works.

That extends to what is missing. `reference/targets.md` says plainly which targets have never run the
code, and `reference/environment.md` lists what is absent by design. Quietly dropping an
inconvenient line is the failure mode to watch for.
