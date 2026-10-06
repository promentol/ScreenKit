// Throws at the top level, through two frames so the JS stack in the reported
// error has something to show.
function inner() {
  throw new Error('boom from the bundle');
}
function outer() {
  inner();
}
console.log('about to throw');
outer();
