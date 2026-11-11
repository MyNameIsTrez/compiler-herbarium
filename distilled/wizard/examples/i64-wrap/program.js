// The module of the example "i64-wrap", as the engine receives it.
// The validator tracks the type of every value: an i64 is added, then wrapped to an i32.
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     i64.const 4294967296
//     i64.const 7
//     i64.add
//     i32.wrap_i64
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f, 0x03,
  0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x0a, 0x0e, 0x01,
  0x0c, 0x00, 0x42, 0x80, 0x80, 0x80, 0x80, 0x10, 0x42, 0x07, 0x7c, 0xa7, 0x0b,
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
