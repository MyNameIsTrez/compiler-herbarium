// The module of the example "arith", as the engine receives it.
// Straight-line code over the parameters: no control flow, so the side table is empty.
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     local.get 0
//     local.get 1
//     i32.add
//     i32.const 3
//     i32.mul
//     i32.const 2
//     i32.div_u
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01,
  0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x0a,
  0x0f, 0x01, 0x0d, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x41, 0x03, 0x6c, 0x41, 0x02, 0x6e, 0x0b,
]);
let out;
try {
  const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));
  try {
    const r = instance.exports.main(20, 22);
    out = Array.isArray(r) ? r.join(' ') : String(r);
  } catch (e) {
    out = 'trap';
  }
} catch (e) {
  out = 'invalid';
}
console.log(out);
