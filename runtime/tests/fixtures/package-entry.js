// The entry of the fixture packages make-fixtures.mjs writes (packages/*.skpkg).
// Running at all proves the host opened the package and ran the entry its
// manifest names; the read proves the package directory is the asset root --
// `fonts/probe.txt` exists only inside the package.
var bytes = new Uint8Array(__screenkit.readFile('fonts/probe.txt'));
var text = '';
for (var i = 0; i < bytes.length; i++) text += String.fromCharCode(bytes[i]);
console.log('package entry ran; fonts/probe.txt says: ' + text);
