# dom-usage

Reports which host globals a built JS bundle actually uses, so a shim can be scoped to **evidence
rather than guesswork**. Written for M2: `Architecture.md` §3 proposes composing a DOM layer from
WHATWG packages, and this answers the prior question — what does the app in front of us actually call?

```sh
node tools/dom-usage/analyze.mjs <bundle.js> [more.js…] [--json] [--all]
```

Exit status is always 0. This reports; it does not gate.

## How it decides

Free variables — identifiers the bundle **reads but never declares** — via
[`eslint-scope`](https://www.npmjs.com/package/eslint-scope), the same analyzer ESLint uses for
`no-undef`. ES built-ins (`Object`, `Map`, `Promise`, …) are filtered out because Hermes already
provides them; what remains is what the **host** must provide.

Two deliberate choices:

- **Real scope resolution, not a flat binding set.** The shortcut — collect every declared name into
  one set and subtract — silently suppresses a global the moment any function declares a local of the
  same name. On a minified bundle that is a coin flip. `globalScope.through` cannot make that mistake.
- **String call arguments are captured.** `document.createElement("canvas")` is far more actionable
  than "`document` is used". ESLint can tell you `document` is undefined; it cannot tell you *which
  elements you must support*. That gap is the reason this tool exists rather than an eslint config.

## What it sees, and what it cannot

Two passes. **Globals** — free identifiers and the members touched on them directly. **Instances** —
members touched on values the host handed back, by typing a variable from its origin
(`document.createElement("canvas")`, `new Image()`, `canvas.getContext(...)`, a factory's return) and
then following that variable's references through `eslint-scope`.

It follows values into variables. It does **not** follow them into object properties or function
parameters, and that is a real ceiling rather than a rough edge. Lightning is the worked example:

```js
s = r.getContext(e ? "webgl2" : "webgl", t) || r.getContext("experimental-webgl", t);
return s;              // a factory returns it...
this.gl = e;           // ...and the caller parks it on a property, taken as a parameter
```

Every subsequent `gl.*` call happens on `this.gl`, so **the WebGL surface — the largest in the
bundle — does not appear in this report at all.** The DOM surface does, because DOM objects are used
close to where they are made.

Do not read an empty bucket as "unused". For property-held and dynamically-dispatched values the
complement is *runtime tracing*: run the bundle against `Proxy`-wrapped globals and record every
access. That catches what static analysis cannot, at the cost of only seeing paths actually executed.

## Off-the-shelf alternatives

`eslint-scope` (used here), ESLint's own `no-undef` with no configured `env`, `acorn-globals`, and
Babel's `path.scope.globals` all answer "which globals are free". `eslint-plugin-compat` is the
closest packaged fit but keys off browserslist versions rather than a custom host. None of them
bucket by host surface or capture call arguments, which is the part that turns a list into a
shim plan.
