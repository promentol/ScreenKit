import { defineCollection } from 'astro:content'
import { docsLoader, i18nLoader } from '@astrojs/starlight/loaders'
import { docsSchema, i18nSchema } from '@astrojs/starlight/schema'

export const collections = {
  docs: defineCollection({ loader: docsLoader(), schema: docsSchema() }),
  // Declared but unused: Starlight warns about a missing i18n collection, and an
  // empty one is the documented way to say "one language, for now".
  i18n: defineCollection({ loader: i18nLoader(), schema: i18nSchema() }),
}
