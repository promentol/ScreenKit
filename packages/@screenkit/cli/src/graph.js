// The chunk graph, checked at build time.
//
// Every packed chunk is served from the registry pack.js builds, keyed by file
// name, and a URL that is not in it falls through to SystemJS's own loader --
// which would inject a <script> tag on a runtime that has none. So everything a
// chunk can ask for has to be known here, and has to be a packed chunk:
//
//   - its static dependencies, the `System.register([...deps], ...)` array
//   - its dynamic imports. Legacy renders `import('./x.js')` as
//     `context.import('./x.js')`, where `context` is the register factory's
//     second parameter. A literal URL is followed, optional-call forms
//     (`context?.import(...)`, `context.import?.(...)`) included; anything else
//     -- a variable, a template with holes -- cannot be, and fails the build
//     rather than the launch.
//   - every other use of `context`. Passed to a function, stored in a variable,
//     destructured: from there an import() is out of sight, so the check would
//     pass without having seen it. Only `context.import(...)` and `context.meta`
//     (Babel's import.meta) are allowed, and a destructured context parameter
//     is refused outright.
//
// The chunks are parsed by the pinned hermesc (-dump-ast) and walked with just
// enough scope tracking to tell `context` from a minifier's reuse of the same
// one-letter name in an inner function. Minifiers do reuse it: terser output
// shadows the factory's own parameters routinely.

import { existsSync, readFileSync } from 'node:fs'
import { join, posix } from 'node:path'

import { BundleError } from './errors.js'
import { dumpAst } from './hermesc.js'

/**
 * Check every packed chunk's static and dynamic imports resolve to packed
 * chunks. Throws one BundleError listing every problem found.
 *
 * @returns {{ edges: Array<{ from: string, to: string, dynamic: boolean }> }}
 */
export function checkChunkGraph(hermesc, dist, layout) {
  const packed = new Set(layout.chunks.map((c) => posix.join(layout.chunkDir, c)))
  const problems = []
  const edges = []

  for (const file of packed) {
    const source = readFileSync(join(dist, file))
    const ast = dumpAst(hermesc, dist, file)
    const where = (node) => `${file}:${position(source, node.range?.[0] ?? 0)}`

    const registers = findRegisterCalls(ast)
    if (registers.length !== 1) {
      problems.push(
        registers.length === 0
          ? `${file} does not call System.register, so it is not a legacy chunk the runtime can load`
          : `${file} calls System.register ${registers.length} times; a legacy chunk registers exactly once`,
      )
      continue
    }
    const { name, deps, factory } = registerArguments(registers[0])

    if (name !== null) {
      problems.push(
        `${where(registers[0])}: System.register(${JSON.stringify(name)}, ...) is a named registration; the ` +
          'packed loader serves anonymous chunks only (plugin-legacy emits no other kind)',
      )
      continue
    }
    if (!deps || !factory) {
      problems.push(
        `${where(registers[0])}: System.register is not called as (deps array, factory function), ` +
          'so the chunk\'s imports cannot be checked',
      )
      continue
    }
    for (const dep of deps.elements) {
      const specifier = literalString(dep)
      if (specifier === null) {
        problems.push(`${where(dep ?? deps)}: a System.register dependency is not a string literal`)
        continue
      }
      const target = resolveChunk(file, specifier)
      if (!target || !packed.has(target)) {
        problems.push(
          `missing chunk ${target ?? specifier}: ${file} registers a dependency on ${JSON.stringify(specifier)}, ` +
            notPackedBecause(dist, layout, target),
        )
        continue
      }
      edges.push({ from: file, to: target, dynamic: false })
    }

    const contextParam = factory.params[1]
    if (contextParam !== undefined && contextParam.type !== 'Identifier') {
      problems.push(
        `${where(contextParam)}: the register factory's context parameter is destructured ` +
          `(${snippet(source, contextParam)}), so the chunk's import() calls cannot be checked`,
      )
      continue
    }

    for (const site of findContextImports(factory)) {
      const call = snippet(source, site.node)
      if (site.kind === 'escape') {
        problems.push(
          `${where(site.node)}: the import context \`${site.name}\` is used as a value (${snippet(source, site.parent ?? site.node)}), ` +
            'so an import() made through it cannot be checked. Call import() directly instead.',
        )
        continue
      }
      if (site.kind === 'native') {
        problems.push(
          `${where(site.node)}: a native import() survived the legacy build (${call}); ` +
            'ScreenKit has no module loader to serve it',
        )
        continue
      }
      const specifier = site.kind === 'call' ? literalString(site.node.arguments[0]) : null
      if (specifier === null) {
        problems.push(
          `unresolvable import() in ${where(site.node)}: ${call} -- the chunk it loads is not a literal, ` +
            'so the build cannot pack it. Import a literal path (import(\'./pages/Page.js\')) instead.',
        )
        continue
      }
      const target = resolveChunk(file, specifier)
      if (!target || !packed.has(target)) {
        problems.push(
          `unresolvable import() in ${where(site.node)}: ${call} loads chunk ${target ?? specifier}, ` +
            notPackedBecause(dist, layout, target),
        )
        continue
      }
      edges.push({ from: file, to: target, dynamic: true })
    }
  }

  if (problems.length > 0) {
    throw new BundleError(`the chunk graph does not resolve:\n  ${problems.join('\n  ')}`)
  }
  return { edges }
}

/** Why a resolved chunk is not among the packed ones: absent, or present but outside the chunk directory. */
function notPackedBecause(dist, layout, target) {
  if (target && existsSync(join(dist, target))) {
    return `which is in ${dist} but not in the entry's directory, ${layout.chunkDir}/ -- chunks must sit beside the entry`
  }
  return `which is not a chunk in ${dist}`
}

// ---------------------------------------------------------------------------
// AST reading
// ---------------------------------------------------------------------------

const FUNCTIONS = new Set(['FunctionExpression', 'FunctionDeclaration', 'ArrowFunctionExpression'])

/** Child nodes and node arrays of `node`, skipping position data. */
function* children(node) {
  for (const key in node) {
    if (key === 'range' || key === 'loc' || key === 'type') continue
    const value = node[key]
    if (value !== null && typeof value === 'object') yield value
  }
}

function findRegisterCalls(ast) {
  const found = []
  const stack = [ast]
  while (stack.length > 0) {
    const node = stack.pop()
    if (Array.isArray(node)) {
      for (const n of node) if (n !== null && typeof n === 'object') stack.push(n)
      continue
    }
    if (
      node.type === 'CallExpression' &&
      node.callee?.type === 'MemberExpression' &&
      !node.callee.computed &&
      node.callee.object?.type === 'Identifier' &&
      node.callee.object.name === 'System' &&
      node.callee.property?.name === 'register'
    ) {
      found.push(node)
    }
    for (const child of children(node)) stack.push(child)
  }
  return found
}

/** `System.register([name,] deps, factory)`. `name` is null for the anonymous form. */
function registerArguments(call) {
  const args = call.arguments
  const name = literalString(args[0])
  const offset = name !== null ? 1 : 0
  const deps = args[offset]?.type === 'ArrayExpression' ? args[offset] : null
  const factory = FUNCTIONS.has(args[offset + 1]?.type) ? args[offset + 1] : null
  return { name, deps, factory }
}

const CALLS = new Set(['CallExpression', 'OptionalCallExpression'])
const MEMBERS = new Set(['MemberExpression', 'OptionalMemberExpression'])

/**
 * Every use of the factory's `context`: `context.import(...)` calls ('call'),
 * the method taken without a direct call ('reference', cannot be followed), any
 * other use as a value ('escape', cannot be followed either), plus any native
 * import() left in the body ('native'). `context.meta` is fine and not reported.
 */
function findContextImports(factory) {
  const contextParam = factory.params[1]
  const context = contextParam?.type === 'Identifier' ? contextParam.name : null
  const sites = []

  // [node, shadowed, parent, key]: whether `context` names something else at
  // this point, and where the node sits in its parent.
  const stack = [[factory.body, context === null, null, null]]
  while (stack.length > 0) {
    const [node, outer, parent, key] = stack.pop()
    if (Array.isArray(node)) {
      for (const n of node) if (n !== null && typeof n === 'object') stack.push([n, outer, parent, key])
      continue
    }
    if (node.type === 'ImportExpression') sites.push({ kind: 'native', node })

    const shadowed = outer || declaresInScope(node, context)
    if (!shadowed && CALLS.has(node.type) && isContextImport(node.callee, context)) {
      sites.push({ kind: 'call', node })
      for (const arg of node.arguments) stack.push([arg, shadowed, node, 'arguments'])
      continue
    }
    if (!shadowed && isContextImport(node, context)) {
      sites.push({ kind: 'reference', node })
      continue
    }
    if (!shadowed && node.type === 'Identifier' && node.name === context && isContextValue(parent, key)) {
      sites.push({ kind: 'escape', node, parent, name: context })
      continue
    }
    for (const [childKey, child] of Object.entries(node)) {
      if (childKey === 'range' || childKey === 'loc' || childKey === 'type') continue
      if (child !== null && typeof child === 'object') stack.push([child, shadowed, node, childKey])
    }
  }
  return sites.sort((a, b) => (a.node.range?.[0] ?? 0) - (b.node.range?.[0] ?? 0))
}

function isContextImport(node, context) {
  return (
    MEMBERS.has(node?.type) &&
    node.object?.type === 'Identifier' &&
    node.object.name === context &&
    memberName(node) === 'import'
  )
}

/** A member's property name, when it is a fixed one; null for `x[expression]`. */
function memberName(node) {
  return node.computed ? literalString(node.property) : node.property?.name ?? null
}

/**
 * Is an identifier spelled like `context`, sitting at `parent[key]`, a use of
 * the context as a value? Not when it is only a name: a property name in
 * `x.context` or `{ context: 1 }`, or a label. Not for `context.meta` or another
 * fixed member (import is caught before this). A computed member whose name is
 * not fixed is, since it could be `import`.
 */
function isContextValue(parent, key) {
  if (parent === null) return true
  if (MEMBERS.has(parent.type)) {
    if (key === 'property') return parent.computed
    if (key === 'object') return memberName(parent) === null
  }
  if (parent.type === 'Property' && key === 'key') return parent.computed
  if (key === 'label') return false
  return true
}

/**
 * Does entering `node` bind `name`? Functions bind their parameters, their own
 * name (for an expression) and every `var` or function declared in their body;
 * blocks, loop heads, switches and catch clauses bind their lexical
 * declarations.
 */
function declaresInScope(node, name) {
  switch (node.type) {
    case 'FunctionExpression':
    case 'FunctionDeclaration':
    case 'ArrowFunctionExpression':
      return (
        (node.type === 'FunctionExpression' && node.id?.name === name) ||
        node.params.some((p) => patternBinds(p, name)) ||
        hoistedDeclares(node.body, name)
      )
    case 'ClassExpression':
      return node.id?.name === name
    case 'BlockStatement':
    case 'StaticBlock':
      return node.body.some((s) => lexicallyDeclares(s, name))
    case 'SwitchStatement':
      return node.cases.some((c) => c.consequent.some((s) => lexicallyDeclares(s, name)))
    case 'ForStatement':
      return lexicallyDeclares(node.init, name)
    case 'ForInStatement':
    case 'ForOfStatement':
      return lexicallyDeclares(node.left, name)
    case 'CatchClause':
      return patternBinds(node.param, name)
    default:
      return false
  }
}

function lexicallyDeclares(statement, name) {
  switch (statement?.type) {
    case 'VariableDeclaration':
      return statement.kind !== 'var' && statement.declarations.some((d) => patternBinds(d.id, name))
    case 'ClassDeclaration':
    case 'FunctionDeclaration':
      return statement.id?.name === name
    default:
      return false
  }
}

/** `var` and function declarations anywhere in a function body, not crossing into nested functions. */
function hoistedDeclares(body, name) {
  const stack = [body]
  while (stack.length > 0) {
    const node = stack.pop()
    if (Array.isArray(node)) {
      for (const n of node) if (n !== null && typeof n === 'object') stack.push(n)
      continue
    }
    if (node.type === 'VariableDeclaration' && node.kind === 'var') {
      if (node.declarations.some((d) => patternBinds(d.id, name))) return true
    }
    if (node.type === 'FunctionDeclaration' && node.id?.name === name) return true
    if (FUNCTIONS.has(node.type) || node.type === 'ClassExpression' || node.type === 'ClassDeclaration') continue
    for (const child of children(node)) stack.push(child)
  }
  return false
}

function patternBinds(pattern, name) {
  const stack = [pattern]
  while (stack.length > 0) {
    const node = stack.pop()
    switch (node?.type) {
      case 'Identifier':
        if (node.name === name) return true
        break
      case 'ObjectPattern':
        for (const p of node.properties) stack.push(p.type === 'RestElement' ? p.argument : p.value)
        break
      case 'ArrayPattern':
        for (const el of node.elements) stack.push(el)
        break
      case 'AssignmentPattern':
        stack.push(node.left)
        break
      case 'RestElement':
        stack.push(node.argument)
        break
    }
  }
  return false
}

/** The value of a string literal or a template literal without holes; otherwise null. */
function literalString(node) {
  if (node?.type === 'StringLiteral') return node.value
  if (node?.type === 'TemplateLiteral' && node.expressions.length === 0) return node.quasis[0]?.cooked ?? null
  return null
}

// ---------------------------------------------------------------------------
// Paths and positions
// ---------------------------------------------------------------------------

/**
 * Resolve a specifier the way SystemJS would against the importing chunk's URL,
 * as a dist-relative path. Only relative and root-relative URLs can name a file
 * in the build; anything else (a bare name, a full URL) is null.
 */
function resolveChunk(from, specifier) {
  let resolved
  if (specifier.startsWith('./') || specifier.startsWith('../')) {
    resolved = posix.normalize(posix.join(posix.dirname(from), specifier))
  } else if (specifier.startsWith('/') && !specifier.startsWith('//')) {
    resolved = posix.normalize(specifier.slice(1))
  } else {
    return null
  }
  return resolved.startsWith('../') ? null : resolved
}

/** `line:column`, both 1-based, of a byte offset. */
function position(source, offset) {
  let line = 1
  let lineStart = 0
  for (let i = source.indexOf(0x0a); i !== -1 && i < offset; i = source.indexOf(0x0a, i + 1)) {
    line++
    lineStart = i + 1
  }
  return `${line}:${offset - lineStart + 1}`
}

/** The call site's source text, cut to fit on a line. */
function snippet(source, node) {
  const [start, end] = node.range ?? [0, 0]
  const text = source.subarray(start, Math.min(end, start + 120)).toString('utf8').replace(/\s+/g, ' ')
  return end - start > 120 ? `${text}...` : text
}
