// The DOM shim is one function scope, so a second top-level `function name` or
// `var name` silently replaces the first -- a canvas helper called `finite`
// once turned the CSS parser's numbers into `undefined`. Fail on any repeat.
//
//   node check-shim-names.mjs <dom-shim.js>
import { readFileSync } from 'node:fs'

const source = readFileSync(process.argv[2], 'utf8')
const seen = new Map()
const repeats = []
source.split('\n').forEach((line, index) => {
  const match = /^  (?:function|var) ([A-Za-z_$][\w$]*)/.exec(line)
  if (!match) return
  if (seen.has(match[1])) repeats.push(`${match[1]}: line ${seen.get(match[1])} and line ${index + 1}`)
  else seen.set(match[1], index + 1)
})
if (repeats.length > 0) {
  console.error(`dom-shim.js declares these names twice at the top level:\n  ${repeats.join('\n  ')}`)
  process.exit(1)
}
console.log(`dom-shim.js: ${seen.size} top-level names, no repeats`)
