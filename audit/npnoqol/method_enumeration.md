# method_enumeration — Method enumeration (get_class_methods / log_class_methods)

## How NPNOQOL used it

NPNOQOL added two public introspection helpers to its fork of the header and used them to **resolve obfuscated method names by their stable JVM descriptor at runtime**, then feed the resolved name into the signature-filtered `hook<T>` overload.

**Fork-added API** (present in `npnoqol/ext/vmhook/vmhook.hpp`, ABSENT upstream):
- `vmhook::log_class_methods<T>()` — `npnoqol/ext/vmhook/vmhook.hpp:8757`. Walks `InstanceKlass::_methods` and `VMHOOK_LOG`s each `[idx] name + descriptor`. Pure diagnostic; compiled out in release.
- `vmhook::get_class_methods<T>()` — `npnoqol/ext/vmhook/vmhook.hpp:8811`. Same walk but **returns** `std::vector<std::pair<std::string /*name*/, std::string /*JVM descriptor*/>>` so callers in release builds (where `VMHOOK_LOG` is a no-op) can surface the data through their own channel. `noexcept`, returns empty on any failure (type not registered / class not loaded). Reads `_methods` directly, no JNI.

Both helpers are thin public wrappers over primitives upstream *already exposes privately*: `klass::get_methods_count()` / `klass::get_methods_ptr()` (upstream `vmhook.hpp:2589` / `:2617`), `method::get_name()` / `method::get_signature()` (upstream `:2243` / `:2281`), plus `type_to_class_map` + `find_class` for the `T → InstanceKlass*` lookup.

**Consumer call pattern — resolve-by-descriptor** (`npnoqol/src/feature/impl/module/hypixel_module.cpp`):

- The resolver itself, `hypixel_module.cpp:326-338`:
  ```cpp
  // First method on T whose JVM descriptor equals `signature` (empty if none).  Obfuscated /
  // mixin method names differ per client and per Lunar build, so we resolve by descriptor.
  template<class wrapper_type>
  auto resolve_method_by_signature(const std::string& signature) noexcept -> std::string
  {
      for (const auto& [name, candidate] : vmhook::get_class_methods<wrapper_type>())
          if (candidate == signature)
              return name;
      return std::string{};
  }
  ```
- Call site 1 — `hypixel_module.cpp:366`: resolve `NetHandlerPlayClient.getPlayerInfoMap` by its return descriptor `()Ljava/util/Collection;` (the *name* is obfuscated, the *descriptor* is not), then `hook<sdk::net_handler_play_client>(name, "()Ljava/util/Collection;", &tab_player_info_map_detour)`.
- Call site 2 — `hypixel_module.cpp:379-391`: the tab-name method's descriptor is *computed* per client. Vanilla/Forge is `(<NPI>)Ljava/lang/String;`; Lunar's mixin redirect is `(<GuiPlayerTabOverlay><NPI>)Ljava/lang/String;` (an extra redirected-receiver arg). The descriptor is built from `mapping::*::signature` tokens, fed to `resolve_method_by_signature<sdk::gui_player_tab_overlay>(signature)`, and the resolved name is handed to the signature-filtered `hook<T>`.
- Diagnostic fallback — `hypixel_module.cpp:342-359`: when nothing matched, `report_tab_name_failure()` calls `get_class_methods<sdk::gui_player_tab_overlay>()` and prints every `String`-returning method (`signature.ends_with(")Ljava/lang/String;")`) to in-game chat — because `VMHOOK_LOG` (hence `log_class_methods`) is compiled out at `VMHOOK_DEBUG_LOGS=0`, so the **returning** variant is the only way to introspect in a shipped build.

The underlying need is stated verbatim at `hypixel_module.cpp:324-325`: *"Obfuscated / mixin method names differ per client and per Lunar build, so we resolve by descriptor."* This is the canonical obfuscated-build problem: the name rotates, the descriptor is stable.

## Pain points encountered (the real-world lessons)

### No public method-enumeration API at all
- **evidence:** `npnoqol/ext/vmhook/vmhook.hpp:8811` adds `get_class_methods<T>()` (and `:8757` adds `log_class_methods<T>()`) from scratch. Upstream `vmhook.hpp` has **zero** matches for either symbol — the fork had to author the public enumeration entry point itself. Its body is a near-verbatim copy of the `_methods`-walk loop that upstream already keeps *private* inside `hook<T>` (upstream `vmhook.hpp:7731-7751`).
- **underlying vmhook gap:** Upstream exposes `hook<T>(name, …)` and the signature-filtered `hook<T>(name, signature, …)` (upstream `:7708`), but **both require you to already know the method name**. There is no way to ask "what methods does class T declare?" The raw primitives (`get_methods_count`, `get_methods_ptr`, `is_valid_pointer`, `method::get_signature`) exist but are internal/`hotspot`-namespace plumbing — a consumer would have to hand-roll `find_class(type_to_class_map[typeid(T)])`, the `_methods` Array<T> layout walk, and per-element `is_valid_pointer` guarding just to list methods. The fork proves the demand by reimplementing exactly that.
- **severity:** major

### Cannot hook obfuscated methods by name — only the descriptor is stable
- **evidence:** `hypixel_module.cpp:366` and `:382` never name the target method. They pass only descriptors (`()Ljava/util/Collection;`; a per-client-computed `(...)Ljava/lang/String;`) and recover the name by scanning. The whole `resolve_method_by_signature` indirection (`:326`) exists solely because `hook<T>(name, …)` demands a name the consumer does not have.
- **underlying vmhook gap:** Upstream's hook resolution is name-keyed: the loop at `:7740-7751` matches `method_ptr->get_name() == method_name` first and only *then* (optionally) checks the signature. There is no "hook the (unique) method whose descriptor == S, regardless of name" path — yet that is the exact shape obfuscated/mixin targets require. The signature is necessary but not sufficient as a *selector* upstream; it is only a tiebreaker.
- **severity:** major

### Release builds are blind: log-only diagnostics vanish at VMHOOK_DEBUG_LOGS=0
- **evidence:** `hypixel_module.cpp:340-341`: *"vmhook.log is compiled out in release builds (VMHOOK_DEBUG_LOGS=0), so on failure report the GuiPlayerTabOverlay String-returning methods to chat (silent on success)."* The fork therefore needed BOTH a log variant (`log_class_methods`, `:8757`) for debug AND a data-returning variant (`get_class_methods`, `:8811`) for release, and re-surfaces the list through chat itself (`:349-356`).
- **underlying vmhook gap:** Upstream's *only* introspection channel is `VMHOOK_LOG`, which is a no-op in release. Any upstream "diagnostic helper" that merely logs is useless to a shipped product. Whatever upstream adds for enumeration MUST return data, not just log it; the log-only form is at best a convenience built on top.
- **severity:** major

### First-match-wins is fragile when a descriptor is non-unique
- **evidence:** `resolve_method_by_signature` (`hypixel_module.cpp:330-336`) returns the **first** method whose descriptor matches and silently ignores any others; `report_tab_name_failure` (`:351-356`) conversely dumps *all* `String`-returning methods, implicitly acknowledging that several methods can share the `(...)Ljava/lang/String;` shape on one class.
- **underlying vmhook gap:** With no enumeration helper, the consumer can't cheaply answer "is this descriptor unique on T?" or "give me all matches so I can disambiguate." Upstream offers neither a "count matches" nor an "all matches" query, so the fork falls back to a brittle take-the-first heuristic that can resolve to the wrong overload on a class with several same-descriptor methods (common after obfuscation collapses names).
- **severity:** annoyance

## Upstream improvements this implies

### [S] Public method-enumeration API: `get_class_methods<T>()` + `log_class_methods<T>()`
- **what to add upstream:**
  - `template<class T> auto get_class_methods() noexcept -> std::vector<std::pair<std::string,std::string>>;` returning `(name, JVM descriptor)` for every declared method of `T`'s registered class. `noexcept`; empty vector if `T` is unregistered or the class is not loaded; reads `InstanceKlass::_methods` directly (no JNI), so callable from any thread once the class is loaded.
  - `template<class T> auto log_class_methods() noexcept -> void;` — same walk, but `VMHOOK_LOG`s each entry; thin debug convenience layered on the returning variant.
  - Consider a non-template overload keyed by binary class name (`get_class_methods(std::string_view class_name)`) so callers can introspect a class they have **not** wrapped with `register_class<T>()` — useful when you're still discovering the obfuscated shape.
- **why it removes the pain:** Folds the fork's hand-rolled `_methods` walk into the library, beside the identical private loop in `hook<T>`. Gives obfuscated-build consumers a first-class "what's on this class?" query and a release-safe data channel, eliminating the need to reach into `hotspot`-namespace internals.
- **fork reference:** `vmhook::get_class_methods<T>()` (`npnoqol/ext/vmhook/vmhook.hpp:8811`) and `vmhook::log_class_methods<T>()` (`:8757`) — port both verbatim; they are already HotSpot-generic with no Minecraft specifics.

### [S] Signature-only hook selector: `hook_by_signature<T>(descriptor, detour)`
- **what to add upstream:** `template<class T> auto hook_by_signature(std::string_view jvm_descriptor, auto&& detour) -> bool;` that scans `_methods`, matches purely on `method::get_signature() == jvm_descriptor`, and hooks the match. Reuse the existing install path of `hook<T>(name, signature, …)` (upstream `:7708`) after resolving the name internally. Decide and document the multi-match policy (recommended: refuse + throw/return-false when more than one method shares the descriptor, so callers must disambiguate explicitly).
- **why it removes the pain:** Collapses the consumer's `resolve_method_by_signature(...)` + `hook<T>(name, sig, …)` two-step (`hypixel_module.cpp:366`, `:382-391`) into one call and removes the requirement to know an obfuscated name. Directly serves the "name rotates, descriptor is stable" reality.
- **fork reference:** No single fork symbol — it is the *composition* the consumer keeps writing (`resolve_method_by_signature` at `hypixel_module.cpp:326` feeding the signature-filtered `hook<T>` at `:390`). Upstream should make that composition a first-class call.

### [XS] `find_methods_by_signature<T>(descriptor)` returning all matches
- **what to add upstream:** `template<class T> auto find_methods_by_signature(std::string_view descriptor) noexcept -> std::vector<std::string>;` (names of every method on `T` whose descriptor matches). Trivial filter over `get_class_methods<T>()`.
- **why it removes the pain:** Lets callers detect/handle non-unique descriptors instead of silently taking the first match (`hypixel_module.cpp:330-336`), and replaces the ad-hoc `ends_with(")Ljava/lang/String;")` chat-dump (`:353`) with a supported predicate query.
- **fork reference:** Implicit in `report_tab_name_failure`'s manual filter loop (`npnoqol/src/feature/impl/module/hypixel_module.cpp:351-356`).

## New test scenarios (mostly jvm_integration — exercise the real usage)

### [jvm_integration] enumerate_methods_returns_name_and_descriptor_pairs
- **scenario:** Register a test class with several known methods (incl. overloads and at least one `(...)Ljava/lang/String;` returner). Call `get_class_methods<T>()` once the class is loaded.
- **asserts:** result is non-empty; every known `(name, descriptor)` pair is present; each descriptor is a syntactically valid JVM method descriptor (`(...)…`); no entry has an empty name or descriptor; calling on an **unregistered** type returns an empty vector (not a throw); calling before the class is loaded returns empty.

### [jvm_integration] resolve_obfuscated_method_by_descriptor_then_hook
- **scenario:** Reproduce the real pattern: a class where the target method's *name* is treated as unknown but its descriptor (`()Ljava/util/Collection;`) is known. Scan `get_class_methods<T>()`, pick the name whose descriptor matches, then `hook<T>(name, descriptor, detour)` (or `hook_by_signature<T>(descriptor, detour)` if added). Invoke the Java method.
- **asserts:** exactly one descriptor match found; the resolved name equals the real method name; the hook installs and the detour fires; `verify_hooks` reports the hook healthy.

### [jvm_integration] enumeration_is_release_safe_data_channel
- **scenario:** Build the test with logging disabled (`VMHOOK_DEBUG_LOGS=0`). Call `get_class_methods<T>()`.
- **asserts:** returns fully-populated data even though `VMHOOK_LOG`/`log_class_methods` produce no output — i.e. the data path is independent of the compiled-out log path (this is exactly why the fork needed the returning variant).

### [unit] non_unique_descriptor_policy
- **scenario:** A class declaring two methods that share one descriptor. Call `find_methods_by_signature<T>(descriptor)` and (if added) `hook_by_signature<T>(descriptor, detour)`.
- **asserts:** the finder returns **both** names; the single-target `hook_by_signature` follows the documented multi-match policy (recommended: returns false / throws rather than silently hooking an arbitrary one).

### [unit] enumeration_robust_against_garbage_method_slots
- **scenario:** Drive `get_class_methods<T>()` against a klass whose `_methods` array contains an invalid/garbage `Method*` slot (simulate via a stub that fails `is_valid_pointer`).
- **asserts:** the bad slot is skipped (matching the fork's per-element `is_valid_pointer` guard at `npnoqol/ext/vmhook/vmhook.hpp:8836`); no crash; valid entries still returned; function stays `noexcept`.

## Verdict: should this be ported upstream?

**Yes — port the general mechanism.** Method enumeration by `(name, descriptor)` is a pure HotSpot-introspection primitive with nothing Minecraft-specific in it: the fork's `get_class_methods<T>()` is literally the `_methods`-walk loop upstream already runs privately inside `hook<T>`, just made public and data-returning. The motivating problem — *obfuscated builds rotate method names, so you must select by the stable JVM descriptor* — is universal to anyone hooking obfuscated/minified/shaded JVM bytecode, not unique to Lunar or Hypixel. Upstream should port (1) the returning `get_class_methods<T>()` and its `log_class_methods<T>()` convenience, ideally with a class-name-keyed overload for not-yet-wrapped classes, and (2) the signature-only selector `hook_by_signature<T>(descriptor, …)` that turns the consumer's repeated resolve-then-hook dance into one call. The only thing to deliberately leave behind is the Minecraft glue — the `mapping::*::signature` descriptor-building and the `EnumChatFormatting`/chat surfacing (`hypixel_module.cpp:349-356`) are consumer concerns; upstream exposes the descriptors as data and lets each consumer present them however it likes.
