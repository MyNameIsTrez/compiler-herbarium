// The module of the example "return-nested", as the engine receives it.
// return from inside two blocks with a value left below the result: it needs no side table entry.
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     block
//     block
//     i32.const 5
//     i32.const 6
//     return
//     end
//     end
//     i32.const 0
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f, 0x03,
  0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x0a, 0x11, 0x01,
  0x0f, 0x00, 0x02, 0x40, 0x02, 0x40, 0x41, 0x05, 0x41, 0x06, 0x0f, 0x0b, 0x0b, 0x41, 0x00, 0x0b,
]);
let out;
try {
  const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));
  try {
    const r = instance.exports.main();
    out = Array.isArray(r) ? r.join(' ') : String(r);
  } catch (e) {
    out = 'trap';
  }
} catch (e) {
  out = 'invalid';
}
console.log(out);
