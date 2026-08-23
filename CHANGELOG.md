## 2.0.3

- Unify optimization planning for native types, containers, Callables, and expressions to improve AOT performance.
- Fix Windows performance regressions caused by cross-compiler differences and improve native integer and compound-expression performance.

## 2.0.2

- Share one Host ABI header set across all export targets to reduce plugin size.

## 2.0.1

- Support newer editor versions by falling back to an earlier API profile.
- Fix repeated extension initialization failures and incorrect missing-library export warnings.

## 2.0.0

- Introduce a new AOT engine to improve GDScript compatibility, runtime performance, and stability.
- Improve the export experience with parallel compilation, link-time optimization, and clearer diagnostics.

## 1.9.0

- Add a bundled Ninja 1.13.2 backend for CPU- and memory-bounded parallel compilation, ordered linking, and incremental reuse without additional user setup.
- Regenerate the complete current C++ project state on every export while preserving unchanged files, making Ninja the sole authority for dependency tracking and zero-work rebuilds.
- Run translation and native builds on an isolated worker while streaming monotonic per-file compile and link progress to the responsive editor UI.
- Improve Windows exports with exact MSVC environments, hidden process output capture, UTF-8 response files, Unicode paths, long paths, and reliable Emscripten command handling.
- Strengthen interrupted-build recovery, iOS XCFramework transactions, packaged Ninja validation, and source-free release boundaries.

## 1.8.4

- Add `gdpp-lite.zip` for Godot 4.6-4.7 with macOS and Windows editors plus desktop, Android, and Web export support, excluding Linux and iOS payloads.
- Stabilize repeated exports by isolating workers from changing editor, export, and shader caches and copying only required metadata atomically.

## 1.8.3

- Add unified `gdpp.zip` packages for Godot 4.6-4.7 and `gdpp-all.zip` packages for Godot 4.4-4.7, each covering all supported editors and export platforms.
- Resolve cross-script classes, inheritance, inner classes, enums, constants, static calls, overrides, and name collisions with consistent project-wide identity.
- Refine inferred fields, constants, parameters, default-argument dependencies, and coroutine contracts before generating cross-script ABIs.
- Preserve compiled Script identity across paths, UIDs, threaded loading, `load`/`preload`, properties, signals, construction, attachment, inheritance, rollback, and shutdown.
- Improve Callable, typed Array and Dictionary, enum, PackedArray, nested write, dynamic `len`, operator, and conversion compatibility.
- Preserve asynchronous initialization, awaited `super` calls, coroutine lifetimes, and safe cancellation during script replacement or shutdown.
- Preserve foreign scripts, scenes, nested resources, serialized references, export exclusions, presets, diagnostics, and customer sources during binary-only export.
- Improve cross-platform startup and shutdown safety for generated static state, Script caches, attached instances, and compiler-specific native branches.

## 1.8.2

- Support contextual-keyword iterator names and enum bodies whose opening brace follows the declaration on a new line.
- Resolve global script classes, inner classes, and nested enums consistently in annotations, constructors, member access, containers, and type tests.
- Preserve named script enums as read-only Dictionary values and exported `Variant` properties with their correct runtime and Inspector metadata.
- Preserve compiled Script resources as canonical stateful objects across loading, properties, signals, object passing, type tests, and construction.
- Fix Godot property access, initializer evaluation order, extension rollback, and shutdown behavior across MSVC, Clang, and GCC.

## 1.8.1

- Fix false `GDS5118` errors in nested `if`/`else` control flow whose branches all return, break, or continue.
- Make control-flow operation identities deterministic after optimization and reject malformed graphs without excessive memory allocation.

## 1.8.0

- Standardize generated project libraries on the `gdpp_library_init` entry point and remove obsolete project descriptors and CMake scaffolding during upgrades.
- Support suspending static functions, typed coroutine lambdas, concurrent resumptions, and reference-counted FunctionState results compatible with native GDScript `await`.
- Match GDScript lambda capture, shadowing, shared-container identity, default arguments, varargs, returned Callables, and asynchronous loop behavior.
- Contain fatal script operations to the current generated call while preserving caller execution, source order, lazy evaluation, and exact `.gd` diagnostics.
- Match Dictionary missing-key, stored-null, typed key/value, read-only, named access, direct assignment, and compound assignment behavior.
- Enforce strict runtime storage conversions for Variant, Object, Ref, Array, Dictionary, PackedArray, and attached properties without silently creating default values.
- Improve exact integer, Variant, Dictionary, Callable, and conversion fast paths while keeping AOT performance within the commercial 10% regression limit.
- Make static, preload, field, `_init`, and `@onready` initialization transactional and improve null, freed-object, `Object.free()`, and attached-instance lifetime safety.
- Add end-to-end `breakpoint` support and source-level debugger frames for methods, accessors, lambdas, inherited members, attached classes, and suspended coroutines.
- Improve parser recovery, large-project worker-stack safety, deterministic MIR validation, and transactional optimization failures.
- Preserve dynamic compiled Script identity for runtime paths, UIDs, ResourceLoader calls, cache checks, `exists()`, `get_script()`, and `.new()`.
- Improve ENet reconnect and teardown, awaited assignments and accessors, custom Godot API builds, double precision, and SDK/runtime compatibility validation.

## 1.7.10

- Match inherited `@onready` initialization and repeated `request_ready()` lifecycles for attached scripts without shadowing user `_ready` callbacks.
- Add negative-index-aware bounds checks for Array and every PackedArray read, write, and compound assignment with source-located diagnostics.
- Guard typed and dynamic object calls, properties, keys, iterators, setters, and component write-backs against null or freed receivers.
- Preserve PackedArray element types, shared storage, Signal and Callable arguments, local signal behavior, and direct generated method dispatch.
- Track true generated-header dependencies so implementation-only edits rebuild fewer files while public script changes rebuild all required dependents.
- Preserve coroutine loop-carried state across process-frame and signal suspension and retain legal unused GDScript bindings in warning-clean generated C++.
- Enforce the commercial performance contract that AOT startup, frame work, and benchmark families remain within 10% of GDScript.

## 1.7.9

- Replace the deprecated Node.js 20 MSVC setup dependency with a repository-owned Node.js 24 action and stricter Visual Studio x64 toolchain validation.
- Discover Visual Studio through `vswhere`, support Preview and non-default installations, preserve explicit compiler overrides, and invoke the correct sibling linker.
- Shorten generated customer library filenames from the `gdpp_project` prefix to `gdpp` across desktop, Android, iOS, and Web and remove retired artifacts transactionally.
- Restore the standard `addons/gdpp/` archive root and update packaged editor compatibility to Godot 4.7.1 without strict-project Variant inference warnings.
- Keep editor and provider descriptors immutable during export, avoid invalid hot reloads, and register the generated project library exactly once.
- Preserve correct Android ABI, Web thread mode, desktop destination, iOS entry, and third-party provider packaging through the unified registration path.
- Add true Universal 2 macOS compiler, fallback, and Godot 4.4-4.7 desktop SDK binaries for Apple Silicon and Intel editors and exports.
- Improve interrupted transaction recovery and classify only the exact known Godot 4.6.2 iOS template warning without hiding unrelated diagnostics.

## 1.7.8

- Defer script constants and resource preloads until Godot requests them, preventing exported games from failing while engine services are still starting.
- Keep descriptor registration metadata-only and validate deferred constants, inheritance merges, duplicate names, and missing resolvers without running customer code.
- Restore root and inner attached-class construction and route their fields through typed descriptor getters and setters with custom accessor support.
- Preserve static function Callables as signal targets and retain exact attached Script identity in typed Arrays, Dictionaries, defaults, and cross-script returns.
- Resolve Godot property reads and writes through their actual getter and setter types, including polymorphic Shader, material, particle, sky, fog, geometry, light, decal, and camera resources.
- Separate property getter and setter contracts so assignments and compound write-back use the engine's real write-side type.
- Build desktop host components in parallel with compiler, Godot, Android, Web, and iOS jobs before final package assembly.
- Remove duplicated SDK content by sharing headers, sources, and runtime data while retaining only platform-, architecture-, profile-, and Web-mode-specific libraries.

## 1.7.7

- Move source scanning, parsing, semantic analysis, native generation, compilation, and linking onto a background worker while the editor remains responsive.
- Compile compatible GDScript only when export starts and build exactly the selected Debug or Release target; ordinary editing and in-editor play keep using original scripts.
- Replace customer CMake projects, editor prebuilds, secondary descriptors, and hot-reload chains with one direct export build and one generated project library.
- Use attached script behavior and an export-scoped ScriptLanguage metadata bridge so Godot and third-party GDExtension objects remain the real owners without source changes.
- Preserve cross-script fields, methods, Autoloads, inner classes, inheritance, `is`/`as`, `self`, provider callbacks, properties, signals, and serialized defaults.
- Fix Dictionary named access for JSON and HTTP responses and nested updates so asynchronous login and networking flows retain their values.
- Preserve shared storage for all PackedArray types across aliases, parameters, returns, defaults, Callables, lambdas, signals, subscripts, iteration, and dynamic calls.
- Materialize cross-script and engine-call arguments from the callee's real ABI and use consistent Variant adapters for containers, providers, utilities, and varargs.
- Ship one optimized customer binding for both Debug and Release exports and consolidate distribution into desktop packages with the supported mobile and Web SDKs.
- Improve Windows build reliability, live per-file progress, rendering refresh, install-ready plugin layout, and descriptor isolation from Godot's extension scanner.

## 1.7.6

- Keep the native build window above the active Godot export dialog on every desktop host.
- Replace profile-specific and column-based progress displays with one continuous, monotonic AOT progress bar.
- Allocate progress across scanning, parsing, analysis, script precompilation, native generation, compilation, and linking with per-file subdivisions.
- Show concise AOT, Debug, and Release labels plus live file counters without exposing backend translation terminology.

## 1.7.5

- Fix Windows MSVC environment bootstrapping so `vcvars64.bat`, `cl.exe`, and quoted paths execute correctly.
- Serialize generated translation-unit compilation and linking to prevent export-time process bursts and memory contention.
- Keep the Godot editor responsive and advance progress after every completed native file.
- Hide toolchain windows and surface bounded stdout, stderr, failing files, phases, and exit codes in export diagnostics on every desktop host.

## 1.7.4

- Include the native-build progress interface in every commercial plugin package and reject incomplete release archives.
- Use optimized `template_release` customer bindings for both Debug and Release exports while retaining the private editor binding required by Godot.
- Remove all customer `template_debug` archives to eliminate duplicated debug objects and substantially reduce package size.
- Preserve Debug-export `assert()` behavior while applying Release optimization, dead-code removal, and symbol hiding to generated project libraries.
- Add complete per-Godot-version packages containing macOS, Linux, and Windows editors plus desktop, Android, iOS, and Web export SDKs.
- Reject stale, mixed, incomplete, generated, nested, or Debug-bound SDK content before compilation or release assembly.

## 1.7.3

- Stop Windows compiler, environment, and linker subprocesses from creating visible console windows during export.
- Use the direct native-build API for bounded compilation and ordered linking, removing unused customer CMake export controls.
- Show a dedicated native-build progress display before Godot packaging for scanning, parsing, analysis, native generation, compilation, and linking.
- Keep progress ordered and monotonic across build profiles and remove the native overlay before Godot's packaging progress starts.

## 1.7.2

- Add lossless zero-source-change reflection for third-party GDExtension Variant, typed container, Signal, object, and encoded metadata contracts.
- Fix internal-class overrides, `super`, default arguments, varargs, coroutines, and dynamic dispatch according to the generated native ABI.
- Preserve nested enum identity across declarations, parameters, returns, containers, and cross-script references.
- Compare complete attached-script descriptors and harden Callable assignment against self-aliasing.
- Reject unsupported Windows arm64, Linux arm64, and Android x86_64 targets before building and strengthen Windows SDK validation.

## 1.7.1

- Complete typed variadic functions, constructors, methods, lambdas, reflection, call thunks, caching, and attached dispatch across Godot 4.4-4.7.
- Add cross-script preload namespaces for root and inner classes, enums, constants, statics, typed resources, casts, type tests, and inheritance.
- Improve cross-version RefCounted, Object, singleton, property, null, dynamic-call, and coroutine ABI handling.
- Resolve Autoload UIDs, preserve runtime resource graphs, and isolate editor-only scripts during binary-only export.
- Protect Dictionary, container, string, path, PackedArray, Callable, and Signal assignment from self-assignment corruption and native resource retention.
- Improve cross-platform compiler and SDK propagation and reject incompatible runtime, CRT, exception, platform, architecture, and profile combinations before export.

## 1.7.0

- Add zero-source-change AOT inheritance from independent third-party GDExtension classes without rebuilding or modifying provider plugins.
- Attach generated ScriptInstance behavior to provider-owned native objects for fields, methods, properties, signals, notifications, virtual callbacks, and RPC metadata.
- Preserve script inheritance, initialization, accessors, signals, and RPC overrides above third-party native roots.
- Call external `super` methods through reflected stable GDExtension ABI contracts instead of unsafe cross-library C++ inheritance.
- Convert scenes, resources, embedded resources, and Autoloads for binary-only export while preserving provider types, state, descriptors, and libraries.
- Improve provider load-order independence, cache invalidation, macOS Universal validation, crash recovery, and SDK integrity checks.

## 1.6.0

- Complete Variant, nullability, and truthiness semantics across analysis, typed IR, generated C++17, and the native runtime.
- Define strict assignment, conversion, constructibility, and storage rules for numeric, string, built-in, PackedArray, Object, Ref, RID, Array, and Dictionary values.
- Add parameterized Array and Dictionary casts, cross-script enum casts, guarded runtime conversions, and stable diagnostics for impossible conversions.
- Validate typed-container elements, keys, values, object classes, and script metadata without relying on permissive implicit native conversions.
- Preserve abstract native script contracts in binary-only exports and update every platform SDK for the guarded conversion and storage runtime.

## 1.5.0

- Add flow-sensitive typing for type tests, null and truthiness checks, short-circuit logic, branches, loops, conditional expressions, guards, and structural `match`.
- Preserve effective types, storage types, and non-null proofs across branches while invalidating unsafe assumptions after reassignment or Callable boundaries.
- Add source-located null and freed-object guards for statically resolved methods and properties.
- Complete default arguments and Callable metadata for omitted arguments, overrides, arity checks, and Godot virtual methods.
- Implement GDScript utilities including `assert`, `is_instance_of`, `type_exists`, `convert`, `str_to_var`, and `var_to_str`.
- Enforce static-context rules, fail closed on unresolved symbols, and preserve left-to-right side-effect order in generated expressions.

## 1.4.0

- Unify integer semantics across constant evaluation, optimization, generated typed code, and dynamic runtime operations.
- Define deterministic overflow, shifts, division, modulo, compound assignment, and native range behavior without C++ undefined behavior.

## 1.3.0

- Add native `for` loops for floating-point and Vector2/Vector2i/Vector3/Vector3i ranges with Godot-compatible direction, step, and boundaries.
- Add typed custom iterator protocol support and stricter contextual validation for array, dictionary, and loop-variable types.

## 1.2.0

- Add end-to-end typed Array and Dictionary support across analysis, cross-script interfaces, exported properties, nested containers, and generated C++.
- Validate container storage and construct typed literals directly to reduce allocations, conversions, and wrapper copies.

## 1.1.0

- Expand modern GDScript syntax support for strings, numbers, Unicode identifiers, local constants, trailing commas, and lambdas.
- Add array, dictionary, nested, and rest `match` patterns across analysis, optimization, and native generation.
- Complete `await` and coroutine results, immediate completion, concurrent calls, and inherited dynamic dispatch.
- Improve multi-script dependency tracking, incremental rebuilds, malformed-input safety, diagnostics, and cross-platform export settings.

## 1.0.0

- Initial release with the GDScript AOT compiler and Godot editor plugin.
