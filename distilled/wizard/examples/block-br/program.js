// The module of the example "block-br", as the engine receives it.
// A forward branch to a block's end that is not taken: the br_if entry is skipped.
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     block i32
//     i32.const 100
//     local.get 0
//     br_if 0
//     drop
//     i32.const 200
//     end
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
  0x03, 0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x0a, 0x12,
  0x01, 0x10, 0x00, 0x02, 0x7f, 0x41, 0xe4, 0x00, 0x20, 0x00, 0x0d, 0x00, 0x1a, 0x41, 0xc8, 0x01,
  0x0b, 0x0b,
]);
let out;
try {
  const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));
  try {
    const r = instance.exports.main(0);
    out = Array.isArray(r) ? r.join(' ') : String(r);
  } catch (e) {
    out = 'trap';
  }
} catch (e) {
  out = 'invalid';
}
console.log(out);
