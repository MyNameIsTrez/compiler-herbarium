// The module of the example "if-no-else", as the engine receives it.
// An if without an else: the validator simulates an empty else, so false jumps to the end.
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     local.get 0
//     if
//     i32.const 5
//     local.set 0
//     end
//     local.get 0
//     end
//   func 1 (sig 1)
//     i32.const 0
//     call 0
//     i32.const 1
//     call 0
//     i32.add
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60, 0x01, 0x7f, 0x01, 0x7f,
  0x60, 0x00, 0x01, 0x7f, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69,
  0x6e, 0x00, 0x01, 0x0a, 0x1b, 0x02, 0x0d, 0x00, 0x20, 0x00, 0x04, 0x40, 0x41, 0x05, 0x21, 0x00,
  0x0b, 0x20, 0x00, 0x0b, 0x0b, 0x00, 0x41, 0x00, 0x10, 0x00, 0x41, 0x01, 0x10, 0x00, 0x6a, 0x0b,
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
