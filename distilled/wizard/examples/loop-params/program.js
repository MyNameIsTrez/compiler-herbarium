// The module of the example "loop-params", as the engine receives it.
// A loop with a parameter: a branch to it carries one value (the label arity of a loop is its parameters).
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     local.get 0
//     loop t0
//     local.get 1
//     i32.const 1
//     i32.add
//     local.set 1
//     i32.const 1
//     i32.sub
//     local.tee 2
//     local.get 2
//     br_if 0
//     end
//     drop
//     local.get 1
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
  0x03, 0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x0a, 0x1e,
  0x01, 0x1c, 0x01, 0x02, 0x7f, 0x20, 0x00, 0x03, 0x00, 0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x01,
  0x41, 0x01, 0x6b, 0x22, 0x02, 0x20, 0x02, 0x0d, 0x00, 0x0b, 0x1a, 0x20, 0x01, 0x0b,
]);
let out;
try {
  const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));
  try {
    const r = instance.exports.main(3);
    out = Array.isArray(r) ? r.join(' ') : String(r);
  } catch (e) {
    out = 'trap';
  }
} catch (e) {
  out = 'invalid';
}
console.log(out);
