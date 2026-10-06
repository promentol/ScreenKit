// A failure `screenkit bundle` can explain. Its message is the whole report --
// what is wrong, where, and what to do -- and the CLI prints it and exits
// non-zero. Anything else that escapes is a bug and keeps its stack.
export class BundleError extends Error {
  constructor(message, options) {
    super(message, options)
    this.name = 'BundleError'
  }
}
