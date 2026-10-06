// @ts-check
import { defineConfig } from 'astro/config'
import starlight from '@astrojs/starlight'

// The ScreenKit documentation site. Content lives in src/content/docs; the
// sidebar below is the only place the order of the guide is decided.
export default defineConfig({
  site: 'https://screenkit.bynarek.com',
  vite: {
    build: {
      rollupOptions: {
        // Starlight's components carry `use astro:head-inject`, which Rollup
        // reports as a directive it may not preserve when it bundles the MDX
        // page that imports them. Astro reads that directive at build time, not
        // at run time, so there is nothing to preserve. Suppressed by code and
        // by directive so any *other* module-level directive still gets through.
        onwarn(warning, warn) {
          if (
            warning.code === 'MODULE_LEVEL_DIRECTIVE' &&
            String(warning.message).includes('astro:head-inject')
          ) {
            return
          }
          warn(warning)
        },
      },
    },
  },
  integrations: [
    starlight({
      title: 'ScreenKit',
      description:
        'Run an ordinary web build on TV hardware: Hermes, WebGL and a DOM shim over SDL3, on tvOS, Android TV, Fire TV and embedded Linux.',
      tableOfContents: { minHeadingLevel: 2, maxHeadingLevel: 3 },
      sidebar: [
        {
          label: 'Start here',
          items: [
            { label: 'What ScreenKit is', slug: 'start/what-screenkit-is' },
            { label: 'Quickstart', slug: 'start/quickstart' },
            { label: 'How a build becomes a package', slug: 'start/build-pipeline' },
          ],
        },
        {
          label: 'Guides',
          items: [
            { label: 'Canvases and WebGL', slug: 'guides/canvases' },
            { label: 'Video', slug: 'guides/video' },
            { label: 'Embedding apps with <iframe>', slug: 'guides/iframes' },
            { label: 'The CSS subset', slug: 'guides/css' },
            { label: 'Input: remotes and gamepads', slug: 'guides/input' },
            { label: 'Networking', slug: 'guides/networking' },
          ],
        },
        {
          label: 'Examples',
          items: [{ autogenerate: { directory: 'examples' } }],
        },
        {
          label: 'Reference',
          items: [
            { label: 'The environment contract', slug: 'reference/environment' },
            { label: '@screenkit/shaka', slug: 'reference/shaka' },
            { label: '@screenkit/vite-plugin', slug: 'reference/vite-plugin' },
            { label: 'screenkit bundle', slug: 'reference/cli' },
            { label: 'Targets and what runs where', slug: 'reference/targets' },
          ],
        },
      ],
    }),
  ],
})
