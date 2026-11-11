// The module of the example "if-else", as the engine receives it.
// Both arms of an if that takes a parameter: false jumps past the else opcode, true falls through to the else, which jumps to the end.
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     i32.const 100
//     local.get 0
//     if t0
//     i32.const 1
//     i32.add
//     else
//     i32.const 2
//     i32.sub
//     end
//     end
//   func 1 (sig 1)
//     i32.const 1
//     call 0
//     i32.const 0
//     call 0
//     i32.sub
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60, 0x01, 0x7f, 0x01, 0x7f,
  0x60, 0x00, 0x01, 0x7f, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69,
  0x6e, 0x00, 0x01, 0x0a, 0x1f, 0x02, 0x11, 0x00, 0x41, 0xe4, 0x00, 0x20, 0x00, 0x04, 0x00, 0x41,
  0x01, 0x6a, 0x05, 0x41, 0x02, 0x6b, 0x0b, 0x0b, 0x0b, 0x00, 0x41, 0x01, 0x10, 0x00, 0x41, 0x00,
  0x10, 0x00, 0x6b, 0x0b,
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
