// The module of the example "fib", as the engine receives it.
// Calls: every frame has its own pc and side table pointer.
//
// Source of each function (the bytes below are the same code, plus the module sections):
//   func 0 (sig 0)
//     local.get 0
//     i32.const 2
//     i32.lt_s
//     if i32
//     local.get 0
//     else
//     local.get 0
//     i32.const 1
//     i32.sub
//     call 0
//     local.get 0
//     i32.const 2
//     i32.sub
//     call 0
//     i32.add
//     end
//     end
//   func 1 (sig 1)
//     i32.const 5
//     call 0
//     end
//
// Runs `main` with the arguments of the example and prints what the C entry prints: the
// results separated by spaces, `trap` if execution traps, or `invalid` if validation fails.
const bytes = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60, 0x01, 0x7f, 0x01, 0x7f,
  0x60, 0x00, 0x01, 0x7f, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07, 0x08, 0x01, 0x04, 0x6d, 0x61, 0x69,
  0x6e, 0x00, 0x01, 0x0a, 0x25, 0x02, 0x1c, 0x00, 0x20, 0x00, 0x41, 0x02, 0x48, 0x04, 0x7f, 0x20,
  0x00, 0x05, 0x20, 0x00, 0x41, 0x01, 0x6b, 0x10, 0x00, 0x20, 0x00, 0x41, 0x02, 0x6b, 0x10, 0x00,
  0x6a, 0x0b, 0x0b, 0x06, 0x00, 0x41, 0x05, 0x10, 0x00, 0x0b,
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
