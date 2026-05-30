# field_proxy_get_primitive_float_double

## Summary
Audited `field_proxy::get()` for the JVM `F` and `D` signature branches at
`vmhook.hpp:11087-11148`. The bit-exact read itself is correct (memcpy
preserves NaN/Inf/denormals on every supported platform), but the
**value_t conversion path that fires immediately after** has two real
problems for float/double: implicit conversion of a NaN or Inf to an
integer target type triggers undefined behaviour, and `static_cast<bool>`
on float NaN returns `true` which silently mis-reports values like
`Float.NaN` as a "set" boolean. The bigger concern is parity / coverage:
no standalone unit test exists for the `F`/`D` get path at all (everything
runs only in the JVM-attached example), and no integration test covers
the denormal or signaling-NaN bit patterns that the doc-comment implies
are preserved.

## Bugs

### [medium] static_cast<int>(NaN/Inf) on F/D field is undefined behaviour
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:10943-11004
- **description:** When a user reads a float/double field whose value is
  `NaN` or `+/-INF` and assigns it to an integer target (`int hp =
  proxy->get();`), `cast_for_variant<int, float>` falls into the generic
  `else if constexpr (requires { static_cast<target_type>(value); })`
  branch on line 11001 and executes `static_cast<int>(nan_value)`. Per
  C++ [conv.fpint]/1, converting a floating-point value whose truncated
  integer part cannot be represented by the destination type is **undefined
  behaviour**; modern MSVC returns `INT_MIN`, clang returns `0` or
  `INT_MIN` depending on optimisation, and gcc may emit a poison value.
  Combined with the `noexcept` operator that swallows everything, the
  user has no chance to see this gone wrong. The fix is to range-check
  the source in the fp-to-integral branch and return `0` (or a
  saturating clamp) with a `VMHOOK_LOG` diagnostic when the source is
  not finite or is outside `[INT_MIN, INT_MAX]`.
- **repro:** A field declared `public static float poison =
  Float.NaN;` exposed by a wrapper as `auto get_poison() -> int {
  return get_field("poison")->get(); }` returns an unpredictable value
  depending on compiler / optimisation level. The example fixture
  already has `Example.floatNaN`, `Example.doubleNaN`,
  `Example.floatPosInf` etc. — just call the existing getter and
  assign to `int` to reproduce.
- **suggested_fix:** In `cast_for_variant`, add an `else if constexpr
  (std::is_floating_point_v<clean_source_type> &&
  std::is_integral_v<clean_target_type>)` branch BEFORE the generic
  `static_cast` branch. Use `std::isfinite(value)` and bounds-check
  against `std::numeric_limits<target_type>::min()/max()` before the
  cast; on failure log and return `target_type{}`.
- **confidence:** certain

### [low] static_cast<bool>(float_nan) silently returns true
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11001-11003
- **description:** Same `cast_for_variant` generic branch: when the
  stored variant alternative is `float` or `double` and the target is
  `bool`, `static_cast<bool>(nan)` produces `true` (any non-zero fp
  value, including NaN, is truthy under the standard fp-to-bool rule).
  The user's wrapper `bool isEnabled() { return
  get_field("threshold")->get(); }` on a `float` field whose value is
  currently `Float.NaN` therefore quietly returns `true` even though
  the intent of NaN is "no usable value". The library doc-comment on
  lines 10800-10803 explicitly advertises `bool b = proxy->get();` as
  a supported pattern, so it's reasonable for users to trip this. Fix
  is the same special-case slot as above — for fp-to-bool, return
  `value != 0 && std::isfinite(value)` or log+return false.
- **repro:** `vmhook::field_proxy proxy{ /*ptr to NaN*/, "F", false };
  bool b = proxy.get();` evaluates to `true` even though no sensible
  semantic of "truthy" applies to NaN.
- **suggested_fix:** Combine with the integral-target branch above —
  treat any fp-source-to-integral-or-bool target as "non-finite or
  out-of-range fails" and return zero with a VMHOOK_LOG warning.
- **confidence:** likely

### [low] Misleading get() doc-comment: NaN/Inf preservation is not actually exercised
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11077-11086
- **description:** The doc-comment on `get()` says the dispatcher
  "selects how many bytes to read and which variant alternative to
  populate". For F/D the memcpy is bit-exact, but there is **no unit
  test in tests/** that proves NaN, +Inf, -Inf, signaling-NaN or
  denormals survive the round trip. The JVM-integration test
  (`example.cpp:2382-2387`) checks only `std::isnan()` for the
  default `Float.NaN` payload — it never verifies the **bit pattern**
  (which matters for sNaN bit 22, payload-encoding NaN, and Java's
  contract that `Float.NaN` is a specific quiet-NaN pattern). On
  hosts that ever introduce a `Float.intBitsToFloat(0x7f800001)` -
  style payload, a silent f32-to-double-via-FPU promotion in the
  conversion path could canonicalise the NaN. Not a code bug today,
  but the missing test coverage is what lets bugs like the
  `static_cast<int>(NaN)` one above hide.
- **repro:** N/A — coverage gap, not a behavioural break.
- **suggested_fix:** Add a standalone test (see Tests section below)
  that builds a `field_proxy` over a stack buffer holding specific
  bit patterns, reads back via `get()`, and asserts
  `std::bit_cast<uint32_t>(out) == expected`. Also extend
  `example.cpp::test_edge_primitives` to assert specific bit patterns
  (e.g. `Float.intBitsToFloat(0x7fc00000)` for canonical qNaN).
- **confidence:** certain

## Improvements

### [S] [INTERNAL] Collapse the eight `if (signature_text == "X")` branches into a switch on `signature_text[0]`
- **rationale:** Lines 11095-11147 do eight sequential
  `std::string::operator==(string_view)` comparisons against single-char
  signatures. Each comparison reads the first byte, then compares
  `size() == 1`. A single `if (signature_text.size() == 1) switch
  (signature_text[0]) { case 'F': ... }` dispatches in one branch and
  generates a jump table on every modern compiler. This is the same
  pattern already used by `jvm_primitive_byte_width` at
  vmhook/ext/vmhook/vmhook.hpp:11395-11410 — so the helper exists,
  it's just not reused. Hot-path readers of `field_proxy::get()`
  (every wrapper getter in `example.cpp`) would benefit.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11087-11148
- **suggested_change:** Refactor `get()` to dispatch on
  `signature_text[0]` once via switch; keep the L/[ multi-char fall-
  through as the default. Same number of memcpys, fewer string
  comparisons, much fewer instructions.

### [S] [USER_FACING] Add `get_as<T>()` member for explicit-type extraction
- **rationale:** The user explicitly called out a `method_proxy` vs
  `field_proxy` parity gap. `method_proxy::call()` returns a `value_t`
  that converts to T; `field_proxy::get()` does the same. But there's
  no `get_as<float>()` or `get_as<double>()` shortcut that does
  `static_cast<T>(get())` in one call. The reference inline doc on
  line 1447 even mentions `field_proxy::get_as<T>()` as if it exists,
  but it does not. For F/D specifically this would be useful because
  it would let the user explicitly say "this field is a float, give me
  a double" without going through assignment and triggering the
  implicit conversion chain that hides the NaN bug above.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:1447, 11087-11148
- **suggested_change:** Add `template<typename T> auto get_as() const
  noexcept -> T { return static_cast<T>(this->get()); }` next to
  `get()`. Also remove or fix the stale `get_as<T>()` reference in
  the comment on line 1447 if not implementing it.

### [XS] [USER_FACING] Log when get() is called on a signature with mismatched byte-width
- **rationale:** `field_proxy::set` has a beautiful size-mismatch guard
  (vmhook/ext/vmhook/vmhook.hpp:11236-11246) that fails loudly when
  the user passes the wrong-sized type to set(). `get()` has no
  symmetric protection — if a user constructs a `field_proxy` with
  signature `"F"` but a `field_pointer` that's actually 2-byte
  aligned in heap (which can't happen for HotSpot field offsets,
  but a tester can construct), the 4-byte memcpy will silently cross
  a slot boundary. Adding a `_alignof_addr` debug assertion for F/D
  (`reinterpret_cast<uintptr_t>(ptr) % alignof(float) == 0`) would
  catch test-construction mistakes early.
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11125-11136
- **suggested_change:** Wrap the F/D memcpy with a debug-only check
  like `assert(reinterpret_cast<uintptr_t>(this->field_pointer) %
  alignof(value_type_local) == 0)`. HotSpot already aligns float
  slots on 4 and double slots on 8, so in production this is a no-op.

### [S] [USER_FACING] Add doc-comment to get() explaining NaN/Inf preservation contract
- **rationale:** The current doc-comment (lines 11077-11086) is silent
  on the fp-bit-preservation contract. Users debugging a "why does my
  NaN turn into 0" mystery will go check the doc-comment first. State
  the contract: "memcpy preserves all 32/64 bits including
  NaN-payload, signaling NaN bit, and denormal mantissa; cast to a
  non-floating target may collapse non-finite values to 0".
- **file_lines:** vmhook/ext/vmhook/vmhook.hpp:11077-11086
- **suggested_change:** Add a `@note` block after the type table
  enumerating these guarantees and warning that
  implicit-conversion-to-integer of a non-finite float is unsafe.

## Tests

### [standalone_unit] [new] test_field_proxy_get_float_double_bit_patterns
- **description:** Build a `vmhook::field_proxy` over a stack buffer
  containing specific 4-byte (F) and 8-byte (D) bit patterns, call
  `get()`, and assert the returned value's raw bits match the input
  bits exactly. Covers: +0.0, -0.0, +1.0, -1.0, FLT_MIN,
  FLT_MAX, FLT_TRUE_MIN (denormal), +Inf, -Inf, qNaN
  (0x7fc00000), sNaN (0x7f800001), NaN with payload (0x7fa55555).
  Same patterns at double width.
- **asserts:** `std::bit_cast<std::uint32_t>(static_cast<float>(
  proxy.get())) == expected_bits` for each entry; same for double via
  `std::bit_cast<std::uint64_t>`.

### [standalone_unit] [new] test_field_proxy_get_float_double_alignment
- **description:** Verify the memcpy in get() works correctly at the
  natural alignment for F (4) and D (8). Construct a buffer aligned
  to a 16-byte boundary, then create field_proxies at offsets +0,
  +4, +8 (for F) and +0, +8 (for D). Read each, confirm bit-equal.
  Optional: also exercise an intentionally unaligned offset (e.g. F
  at +1, +2, +3) to confirm `memcpy` doesn't fault — it shouldn't,
  but the test documents the contract.
- **asserts:** All reads return the bit pattern that was written at
  that offset; no crash on misaligned offsets.

### [standalone_unit] [new] test_field_proxy_get_float_to_int_safety
- **description:** Reproduce the `static_cast<int>(NaN)` UB. Construct
  a field_proxy whose F signature points at a buffer holding
  `Float.NaN`, `Float.POSITIVE_INFINITY`, `Float.NEGATIVE_INFINITY`,
  `Float.MAX_VALUE`, `Float.MIN_VALUE`. Assign the `get()` result
  to `std::int32_t`, `std::int64_t`, `bool`, `std::uint32_t`.
  After the suggested fix lands, assert the result is `0` for
  non-finite sources and is in-range for finite ones.
- **asserts:** After fix — non-finite sources convert to integral 0;
  before the fix this test would fail / produce UB.

### [standalone_unit] [extend_existing] test_field_proxy_set_size_guard (extend to F/D)
- **description:** The existing size-guard test
  (tests/test_helpers.cpp:1108-1190) covers I/J/C/ref but not F/D.
  Extend it to assert: (a) `proxy_float.set(double{...})` is refused
  (the 8-byte double would clobber the 4-byte F slot's trailing
  bytes), and (b) `proxy_double.set(float{...})` is refused (the
  4-byte float would leave the high 4 bytes of D unchanged). Both
  catch real user mistakes that the current guard already prevents
  but no test pins down.
- **asserts:** Sentinel bytes around an F field remain intact after a
  `set(double_value)` call; sentinel bytes around a D field remain
  intact after a `set(float_value)` call.
- **existing_file:** tests/test_helpers.cpp:1108-1190

### [jvm_integration] [extend_existing] test_edge_primitives (assert NaN bit pattern, not just isnan)
- **description:** example.cpp's `test_edge_primitives()`
  (vmhook/src/example.cpp:2370-2400) currently checks
  `std::isnan(get_float_nan())` — which is true for *any* NaN bit
  pattern. To prove `field_proxy::get` is bit-preserving, also assert
  that `std::bit_cast<std::uint32_t>(get_float_nan()) ==
  0x7fc00000` (Java's canonical qNaN). Same for
  `std::bit_cast<std::uint64_t>(get_double_nan()) ==
  0x7ff8000000000000`.
- **asserts:** Bit-exact match against Java's canonical NaN constants;
  `std::signbit(get_float_neg_inf())` is true; `+0.0` and `-0.0`
  fields (if added to Example.java) round-trip with correct sign bit.
- **existing_file:** vmhook/src/example.cpp:2370-2400

### [jvm_integration] [new] test_edge_primitives_denormal_subnormal
- **description:** Add `public static float floatDenorm =
  Float.MIN_VALUE; // 1.4e-45 = 0x00000001` to
  `example/vmhook/Example.java` plus a `Double.MIN_VALUE` sibling.
  Expose getters on the wrapper, read from C++ and assert
  `bit_cast` matches `0x00000001` (float) and `0x0000000000000001`
  (double). Catches a hypothetical FPU-promotion path that flushes
  denormals to zero.
- **asserts:** `bit_cast<uint32_t>(get_float_denorm()) == 0x1`;
  same for double `0x1` (53-bit mantissa LSB).

## Parity Concerns
- `method_proxy::value_t` (vmhook.hpp:11439-11495) and
  `field_proxy::value_t` (vmhook.hpp:10809-11061) share almost-identical
  variants and conversion logic but are independent types. The
  fp-to-integer UB documented above exists in BOTH (method_proxy's
  generic `static_cast` branch at lines 11484-11491 has the same
  flaw). A future cleanup could factor the conversion logic into a
  shared helper to fix both sites at once.
- `method_proxy::call_jni`'s F/D return path (vmhook.hpp:12004-12023)
  uses `return value_t{ r }` where `r` is `float`/`double`, exactly
  parallel to `field_proxy::get`'s F/D branches — so the read sides
  match. Good parity.
- `field_proxy::get_compressed_oop` (vmhook.hpp:11303-11313) is a
  dedicated typed accessor for the ref/array case. There is no
  equivalent `field_proxy::get_float()` / `get_double()` /
  `get_int()` etc. dedicated typed accessor — users always go
  through the variant + implicit conversion. A `get_as<T>()` template
  (see Improvements) would close this gap without exploding the API.
- `frame::get_arguments` (referenced in the value_t doc-comment on
  line 1447) is mentioned as a consumer of `value_t` but no
  `get_as<T>()` exists despite the comment implying it does. That
  inconsistency between doc and code is itself a small parity bug.
