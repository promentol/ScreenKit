// `node --test packages/@screenkit/cli/test` hands the runner a directory, which
// node loads as a module -- so this index is what it runs.
import './bundle.test.js'
import './polyfills.test.js'
