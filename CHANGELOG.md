## 1.8.2

- Accept contextual-keyword iterator names and enum bodies whose opening brace follows the declaration on a new line.
- Resolve globally named scripts, their inner classes, and nested enums consistently in type annotations, constructors, member access, and type tests.
- Preserve named script enums as read-only Dictionary values with their declared identity and standard Dictionary behavior.
- Preserve exported `Variant` properties with the correct Inspector, storage, and runtime metadata.
- Preserve compiled Script resources as canonical stateful objects across `load`/`preload`, nullability, Script APIs, properties, signals, object passing, type tests, and construction.
- Route Godot properties without public godot-cpp accessors through the Object property ABI, and preserve receiver/argument evaluation order for calls in field and static initializers.

## 1.8.1

- MIR operation identities are now assigned only after unreachable-block pruning and predecessor reconstruction, in canonical block/instruction/terminator order. Nested `if/else` trees whose branches all `return`, `break`, or `continue` no longer retain holes from discarded join blocks or fail with a spurious `GDS5118`.
- The lowerer and optimizer share one operation-ID canonicalizer, so every CFG-mutating path restores the same deterministic layout. The verifier now rejects missing, duplicate, out-of-range, permuted, and non-dense identities without allowing a corrupt large ID to drive unbounded allocation.

## 1.8.0

- Shorten the generated customer GDExtension C entry ABI to `gdpp_library_init` and enforce that exact symbol across generated registration code, runtime descriptors, Apple embedded registration, and every supported native target.
- Remove the retired generated `gdpp_project.gdextension` and CMake-driven project scaffold during every successful in-place project compilation, so upgrades cannot leave an obsolete entry symbol or inactive build scripts beside the current direct-build manifest.
- Support genuinely suspending static functions with an owner-free FunctionState, typed results, and Signal resumption, and support typed coroutine lambdas with per-call suspension state and independently ordered concurrent resumptions.
- Return every genuinely pending coroutine as a reference-counted FunctionState instead of a raw Signal, with `completed(result)`, `resume(arg)`, `is_valid(extended_check)`, zero/one/many-signal-argument folding, stale-connection cleanup, owner destruction guards, and recognition of both GDPP and native GDScript function states by generated `await`.
- Match GDScript lambda capture semantics with stable local symbol identities: capture values when the Callable is created, retain shared Array/Dictionary/Object identity, create a fresh writable scalar frame for every call, and preserve nested, returned, recursive-container, asynchronous-loop, and debugger-visible captures under lexical shadowing.
- Instantiate Callable creation snapshots through a mutable erased bridge on every supported godot-cpp line, preventing GCC 4.5/4.6 builds from const-qualifying the writable per-call scalar and shared-container frames.
- Introduce script fault frames that stop the current generated function at fatal GDScript operations while preserving caller execution, source order, lazy branches, specialized-call argument order, Callable defaults, and exact `.gd` path/line/column diagnostics across Variant, container, object, and third-party extension boundaries.
- Make synchronous fault frames invocation-local and thread-local while serializing persistent coroutine fault ownership through the FunctionState resume mutex; inline the active-frame checks without allowing a failure marker to cross threads, coroutines, nested calls, or the GDExtension ABI.
- Keep coroutine, coroutine-lambda, and resumed-continuation fault accessors capture-free and resolve them through the state installed on the active thread, so an escaped asynchronous closure can never retain a synchronous `ScriptFunctionScope&`; GCC, Clang, and MSVC runtimes now share the same lifetime contract.
- Classify every typed expression, conversion, and assignment by whether it can set the active script fault, and emit polling only at proven fallible boundaries. Literal and local-value flow, safe integer operations, exact storage, String value methods, and conditional arms no longer accumulate redundant branches, while dynamic calls/operators, strict Variant storage, bounds, division/modulo, Object access, Callable/Signal re-entry, and coroutine boundaries retain their ordered checks.
- Route `Dictionary[key]` and `Dictionary.key` reads through Godot's keyed/named Variant ABI validity result, distinguishing a missing or typed-invalid key from a stored `null` in one lookup. Named reads retain a typed Dictionary's declared value type, reject dot syntax when `StringName` cannot enter the declared key storage, and preserve receiver-first fault order.
- Match direct and compound Dictionary assignment faults for missing keys, typed keys/values, and read-only storage. Named writes use the native `StringName` ABI; direct invalid writes terminate the current function, while a typed compound value rejection preserves the original entry and continues exactly as official GDScript does.
- Add a whole-function, symbol-identity-based proof for borrowed reads and in-place compound updates of existing local untyped Dictionary literal slots. Proven reads retain a `const Variant&` instead of repeating named validity checks and copies; reassignment, aliases, calls, subscripts, unknown keys, typed storage, method access, and closure capture all invalidate the proof and retain the checked runtime path.
- Execute supported exact-integer compound operations directly in an existing `Variant::INT` slot instead of constructing and assigning a result Variant. Both native-integer and dynamic-Variant right sides preserve the shared portable 64-bit overflow, shift, division, and modulo contract; unsupported or non-integer operations retain Godot's general Variant path, and every failure now carries the original `.gd` source location.
- Borrow an existing `Variant` at native-to-Variant boundaries instead of constructing an identical temporary, and transfer each compiler-owned, single-use assignment snapshot through godot-cpp move construction/assignment. Receiver-before-RHS order, fault checks, self-assignment, shared container identity, and checked conversions remain unchanged, while String and dynamic scalar loops no longer pay redundant reference-counted copies.
- Restore the fault-safe backend to the commercial 10% maximum performance-regression contract without relaxing the threshold.
- Encode local lambda arity and vararg identity in the generated C++ type and retain native argument tuples only while the concrete Callable remains inside its creation invocation. Exact parameters avoid Variant round trips, while assigned, escaped, invalid-owner, defaulted, structural, and dynamic paths keep the full Godot Callable and strict Variant checks.
- Keep the exact local-Callable native argument branch structurally exclusive from its strict Variant fallback, preserving the fast path under MSVC's warning-as-error unreachable-code analysis as well as Clang and GCC.
- Fast-path exact explicit Variant conversions before consulting Godot's general conversion matrix.
- Enforce Godot's strict runtime typed-storage conversions for dynamic scalar, Object/Ref, Array/Dictionary, and PackedArray assignments, including attached properties, instead of accepting godot-cpp's more permissive casts or silently materializing default values.
- Make static/preload initialization and attached field, `_init`, and `@onready` phases transactional and independently contained, preventing failed partial state from being published while allowing later object construction and caller code to continue with GDScript-compatible defaults.
- Match Godot's asynchronous engine-virtual ABI: pending callbacks immediately expose their FunctionState to the engine's Variant-to-native conversion, while typed synchronous results, completion, manual resumption, connection cleanup, validity, and continuation execution retain strict semantics.
- Preserve `@static_unload` intent from AST through HIR, generated metadata, project manifests, cache identity, and editor Script descriptors, while matching the Godot 4.4-4.7 observable behavior that script static state remains alive until language/extension shutdown.
- Implement the GDScript `breakpoint` statement end to end through the lexer, parser, semantic model, HIR, typed MIR, verifier, C++17 emitter, and native Godot debugger bridge instead of rejecting a valid production script during AOT compilation.
- Report generated native frames through `ScriptLanguageExtension` with the original `.gd` path, function, current source line, shadow-aware lexical locals, and own plus inherited script members; debugger expressions evaluate against the same frame owner used by the attached script.
- Preserve debugger behavior across ordinary methods, accessors, static functions, lambdas, attached third-party GDExtension bases, and suspended coroutine continuations, while keeping release products free of breakpoint instrumentation and eliding inactive debug frames when no Godot debugger is attached.
- Return the canonical Godot `ScriptInstance` handle from attached native instances so the debugger, engine callbacks, property access, and lifetime tracking all address the same public engine object instead of an internal implementation pointer.
- Defer non-trivial per-thread debugger state until the first Godot callback instead of placing Godot values in the Windows DLL static TLS initialization table, preventing Windows loader error 1114 before the GDExtension entry runs.
- Preserve legal GDScript lexical shadowing in generated C++ under Clang, GCC, and MSVC warning-as-error builds without disabling unrelated native diagnostics.
- Allow macOS Universal debug exports to reuse a verified Release-only third-party provider fat binary when the provider does not ship a distinct debug binary. GDPP adds debug aliases only to the descriptor bytes packaged into the export and never mutates the customer's source descriptor or library.
- Add bounded multi-error parser recovery and preserve complete, monotonic source spans when missing trailing nodes follow unmatched grouping or trailing trivia, including casts inside inline lambdas.
- Run every single-file and project compiler entry point on a GDPP-owned 16 MiB worker stack, isolate the largest call/member semantic frames, and preserve reentrant compilation instead of inheriting Godot script-worker stacks as small as 512 KiB.
- Assign stable, address-free identities to MIR values and operations, serialize versioned control-flow snapshots, validate source ownership and dense predecessor metadata, and make optimization budgets and failures transactional.
- Remove dead MIR values with complete operand remapping while preserving observable debugger, allocation, fault, and suspension operations; keep CSE, inlining, escape, and loop transformations disabled until they have independent equivalence proofs.
- Enforce valid metadata records, argument ranges, property read/write ABIs, Signal contracts, enums/bitfields, scalar returns, and native-pointer boundaries across the Godot 4.4-4.7 API tables.
- Preserve binary-only dynamic Script identity for paths assembled entirely at runtime, including relative paths, UIDs, synchronous and threaded ResourceLoader calls, cache equality, `exists()`, `get_script()`, and `.new()`; paths outside the compiled project manifest fail deterministically.
- Make real ENet reconnect and teardown follow deterministic node/cache, peer, and SceneTree-root ownership: replicated nodes leave the tree before peer replacement or closure, API peer references are cleared, and custom multiplayer roots are then unregistered and freed. Success and abort cleanup are idempotent and no longer trigger duplicate `tree_exited` cache diagnostics on Godot 4.4-4.7.
- A-normalize awaited assignment receivers and indexes before their right-hand side, retaining single evaluation, write-back ownership, lazy branches, and suspension lifetimes for locals, fields, properties, and dynamic subscripts.
- Evaluate omitted default arguments inside the ordered callable prologue, including defaults that genuinely suspend; retain the receiver, prior explicit/default arguments, varargs, and per-call state through resumption.
- Preserve coroutine ABI for inline and method-bound property getters/setters across instance, static, internal-class, cross-script, inherited, and concurrent calls, with transitive project cache invalidation.
- Isolate static coroutine owner state and nested coroutine-lambda continuations from surrounding emitter context so owner-free access, typed returns, and inner failure exits cannot capture an instance or emit an invalid C++ return.
- Reject `await` in annotation constant arguments during semantic analysis rather than allowing an impossible compile-time suspension to reach HIR or C++ generation.
- Lower project-script `Object.free()` through Godot's Variant dispatch instead of `memdelete`, preserving ordinary Object destruction, RefCounted rejection, locked-object protection, freed identity, source locations, and current-function failure semantics.
- Read null and freed Object identities through godot-cpp's exact `Variant::operator ObjectID()` ABI, avoiding MSVC's ambiguous signed/unsigned conversion path without weakening lifetime validation.
- Build custom and double-precision add-ons from an exact exported `extension_api.json`, binding compiler, editor/fallback binaries, godot-cpp, SDK, descriptor features, API SHA-256, precision, and runtime ABI.
- Advance the SDK to schema 12 and the packaged runtime ABI to 18 so SDKs missing the complete debugger, FunctionState, dynamic Script, strict Variant, coroutine-accessor, awaited-default, custom-precision, and Object lifetime contracts fail before any customer compile command is created.

## 1.7.10

- Match GDScript's independent implicit-ready lifecycle for attached scripts: initialize `@onready` fields base-to-derived before normal `_ready` dispatch, including scripts without a user `_ready`, inherited callbacks, internal classes, and repeated `request_ready()` cycles.
- Keep implicit-ready initialization separate from reflected user methods and place every lifecycle hook in the compiler-reserved `_gdpp_` namespace, so customer methods cannot collide with runtime initialization and a derived script with `@onready` fields cannot shadow a real inherited `_ready` callback.
- Remove both the declaration and definition of synthesized `_ready` from onready-only attached classes, preventing unresolved virtual symbols during final customer-library linking.
- Route statically typed `Array` and every `PackedArray` subscript read, write, and compound assignment through negative-index-aware bounds checks while preserving receiver-before-index evaluation; invalid message data now emits a bounded diagnostic with the original `.gd` path, line, column, and operation instead of dereferencing native storage and terminating the process.
- Guard statically typed object property writes, script setters, and built-in component write-backs against null or freed receivers; message-driven UI updates now stop with a source-located Godot error instead of dereferencing an invalid native object. Compound property writes capture the receiver, current value, and right-hand side exactly once in source order, closing side-effect and use-after-free windows during gift-driven state changes.
- Reject null or freed object targets before every dynamic `Variant` call, property/key operation, and iterator step, so malformed protocol objects cannot reach Godot's low-level Variant dispatch with an invalid native pointer.
- Return statically typed `Array` and every `PackedArray` subscript through their native element representation after bounds validation, eliminating per-read `Variant` round trips while retaining attached-object casting, shared-container identity, negative indexes, and source-located failure behavior.
- Pass Signal, Callable, Object-call, and utility varargs through a single-construction adapter, so godot-cpp creates each required `Variant` exactly once while shared `PackedArray` arguments continue to expose their retained identity instead of copying native storage.
- Preserve the native element type through the final generated `PackedArray` read boundary instead of reclassifying the checked value as a `Variant`, covering ordinary reads and compound assignments across every packed element family.
- Emit locally declared signals through a cached signal-name `Variant` and the public Object MethodBind ABI, retaining Godot connection semantics and source-order argument evaluation without reconstructing immutable call metadata on every event.
- Store every validating generated method entry beside its reflected attached-script method and dispatch engine-to-script callbacks directly after ScriptInstance resolution; signals, lifecycle hooks, Callables, animations, and timer callbacks no longer wrap the hidden behavior in a temporary `Variant` or repeat ClassDB lookup.
- Tighten the commercial performance contract to a maximum 10% AOT regression against GDScript for startup, steady-frame work, and every benchmark family.
- Track each generated translation unit's transitive quoted-include graph in the direct native builder instead of making every object depend on every generated header; implementation-only changes now rebuild their own object, while public script-shape changes rebuild only true dependents and the registration unit before relinking.
- Preserve a strong, correctly typed `RefCounted` reference when attached-script `self` crosses a typed native call boundary, covering nested message objects and generated protocol models without unsafe owner conversion.
- Compare statically typed engine objects, `RefCounted` values, and provider-owned attached `self` through Godot Variant identity, avoiding ill-formed `Ref<T> == Object*` code in exported protocol and model classes.
- Advance the packaged runtime ABI to 13 so an older or partially copied SDK is rejected during export preflight instead of linking lifecycle- or signal-call-incompatible generated code.
- Install coroutine loop-carried storage before emitting asynchronous `while` conditions or `for` iterables, guaranteeing that conditions, bodies, and resumed continuations observe one native value after process-frame or signal suspension instead of leaving batched background work on a stale copy.
- Preserve warning-clean C++ without deleting source-level bindings: unused method, setter, lambda and rest parameters, locals, iteration and match bindings, and await payloads now remain available with their exact GDScript lifetime and evaluation semantics.

## 1.7.9

- Replace the Node.js 20-based `ilammy/msvc-dev-cmd` dependency with a repository-owned, zero-dependency Node.js 24 Action that discovers Visual Studio through `vswhere`, initializes the supported MSVC x64 toolchain, exports only changed environment variables, preserves values containing `=`, deduplicates Windows tool paths, and fails closed on incomplete or mismatched toolchain state.
- Upgrade the workflow semantic validator to actionlint 1.7.12 with native Node.js 24 action-metadata support, and use the repository-owned Node.js 24 bootstrap for compiler-core, native-integration, and commercial Windows host builds.
- Pass the Node.js 24 MSVC initialization payload to `cmd.exe` without generic Windows argument re-quoting, preserving quoted Visual Studio paths under `Program Files`.
- Shorten every generated customer runtime artifact from the `gdpp_project` filename prefix to `gdpp` across Windows, macOS, Linux, Android, iOS, and Web, including the Windows import library, Mach-O install name, iOS slice library, and XCFramework layout, while retaining the stable `gdpp_project_library_init` GDExtension entry ABI.
- Advance the native-build and export-transform revisions and transactionally remove artifacts generated with the retired filename before the next AOT export, including directory-form iOS XCFrameworks; fail closed if a stale product cannot be removed.
- Update desktop runtime, APK, XCFramework, Wasm, PCK, plugin-package, and cleanup logic to require the shortened names, reject generated customer products and retired products from commercial plugin archives, and preserve compiler/fallback binaries through exact product classification.
- Restore the standard `addons/gdpp/` root in all three desktop plugin ZIPs so extracting an archive directly into a Godot project produces a discoverable EditorPlugin instead of an inert `res://gdpp/` directory, and advance the package manifest to schema 5.
- Update packaged editor compatibility to official Godot 4.7.1 and eliminate Variant-inference warnings that break strict customer project settings.
- Make the production Windows exporter discover Visual Studio through `vswhere`, require the installed x64 C++ tools component, accept preview and non-default installations, preserve explicit compiler overrides, remove the hard-coded user-profile fallback, and report an actionable toolchain diagnostic instead of misclassifying an installed MSVC environment as missing.
- Resolve the selected MSVC `cl.exe` to an absolute Visual Studio tool path before native-build planning and invoke the sibling `link.exe` explicitly; inherited developer environments can no longer redirect customer links to Git/MSYS's unrelated `link.exe`, while direct process execution retains the same fail-closed defense.
- Keep the editor compiler descriptor immutable during successful AOT exports and mark it non-reloadable to match its actual godot-cpp build contract, eliminating GDExtension instance-recreation failures caused by replacing a loaded compiler extension with the generated project runtime.
- Run GDPP's export plugin before Godot's built-in GDExtension scanner, replace only the packaged descriptor bytes at the stable `addons/gdpp/gdpp.gdextension` path, and register the generated project library exactly once through the public export API.
- Preserve platform-specific native packaging while using that single registration path: Android receives the exact ABI tag, Web receives the selected thread feature, desktop libraries retain their platform destinations, and iOS receives the Godot 4.4 or 4.5+ static-entry callback and undefined-symbol linker contract required by its XCFramework.
- Resolve macOS Universal 2 third-party GDExtension libraries and dependencies entirely in memory, verify fat Mach-O payloads, package the provider's byte-for-byte original descriptor, and never rewrite customer extension files for architecture discovery.
- Limit the remaining extension-registry transaction to intentional source-only or non-desktop fallback exports where Godot's forced metadata pass must exclude the runtime descriptor; normal AOT exports keep both the registry and every extension descriptor byte-stable.
- Recover interrupted descriptor transactions left by pre-1.7.9 builds at editor startup, while new exports no longer create compiler or provider descriptor backups.
- Keep Visual Studio discovery C++17-compatible by handling UTF-8 `vswhere` output without the C++20 `std::string::starts_with` API.
- Upgrade `gdpp-mac.zip` from an arm64-only host contract to true Universal 2 compiler, fallback, and Godot 4.4-4.7 desktop SDK binaries; both Apple Silicon and Intel editors now load the same plugin, and the standard official Universal 2 export template works without custom templates or preset rewrites.
- Require arm64 and x86_64 slices plus the macOS 11.0 deployment target in every shipped compiler, fallback, and desktop godot-cpp archive.
- Classify Godot 4.6.2's exact upstream iOS-template warning for the removed optional `application/boot_splash/fullsize` setting without changing customer projects or accepting unrelated warnings and errors.

## 1.7.8

- Keep attached-script descriptor registration strictly metadata-only: script constants, including nested resource containers and otherwise pure values, are represented by captureless deferred resolvers instead of being evaluated while GDExtension classes are registered.
- Materialize deferred constants only when Godot requests the compiled Script constant map, preserving local and inherited constant reflection, derived shadowing, deterministic descriptor identity, thread-safe generated constant caching, and explicit shutdown cleanup.
- Prevent exported games from terminating during native extension startup when a script constant preloads a scene or resource whose construction depends on services that are not yet initialized, including 2D/3D physics, rendering, audio, navigation, and third-party GDExtension services.
- Validate deferred resolver presence, eager/deferred name conflicts, inheritance merges, and duplicate descriptors fail-closed without invoking customer code or engine resource services under the registry lock.
- Guard the remaining editor-only Engine lookup during generated project registration so an unavailable mandatory singleton produces a bounded initialization error instead of a native null dereference.
- Restore runtime construction for root and inner attached classes by assigning the resolved descriptor contract to every generated Script before attachment; failed `.new()` construction now emits an actionable diagnostic instead of degrading into later null property failures.
- Dispatch attached ScriptInstance fields through descriptor-owned typed getter/setter callbacks, preserving custom accessors, Variant conversion, inherited properties, and typed-container keys without depending on ClassDB property reflection for GDExtension behavior objects.
- Advance the packaged runtime ABI to 11 so stale SDKs fail preflight instead of compiling against the old eager descriptor layout.
- Resolve Godot properties through their getter/setter ABI instead of the first inspector resource alternative, preserving polymorphic assignments and reads for shader, particle, sky, fog, geometry, light, decal, camera-attribute, and related resource properties across Godot 4.4–4.7.
- Treat owner-free static function Callables as valid signal targets for the lifetime of the Callable while retaining Object lifetime checks for instance-bound lambdas, restoring bound asynchronous callbacks such as `HTTPRequest.request_completed`.
- Preserve the provider-owned native base and exact canonical Script resource for attached root and inner classes used by typed Arrays and Dictionaries, preventing valid compiled objects from being rejected as plain `RefCounted` or third-party GDExtension instances.
- Canonicalize short, qualified, and generated native aliases of the same script class before forming typed-container C++ identities, keeping fields, function returns, chained calls, and locals on one ABI-stable Array/Dictionary specialization.
- Decouple canonical typed-container Script resources from generated descriptor registration order, allowing cross-script defaults to reserve their exact identity before the referenced descriptor is registered and bind its contract later without startup errors or untyped fallback.
- Enforce polymorphic resource-property accessor contracts across all ShaderMaterial-compatible canvas, particle, fog, sky, geometry, CSG, mesh, and tile material slots in Godot 4.4–4.7.
- Model Godot property getter-return and setter-argument ABIs as separate semantic and HIR contracts; assignment diagnostics, compound writeback, and C++ argument materialization now use the actual write-side type instead of assuming current engine accessors are permanently symmetric.
- Run the macOS, Linux, and Windows commercial host-component builds in parallel with compiler, Godot, Android, Web, and iOS jobs; final package assembly starts only after every required producer succeeds.
- Separate desktop component production, release orchestration, and three-package aggregation into reusable workflows with explicit dependencies and a single release entrypoint.
- Flatten every packaged Godot-version SDK into one shared runtime, godot-cpp header tree, source tree, and `lib` directory; Android, Web threads/nothreads, iOS, and the package host now contribute only their distinct optimized Release libraries instead of duplicating the common SDK payload.
- Select target manifests and godot-cpp libraries by exact platform, architecture, profile, and Web thread mode from the shared SDK, while retaining legacy component-layout compatibility and preferring exact macOS slices over Universal fallbacks.
- Ship each desktop archive with `gdpp/` as its top-level directory instead of an `addons/` wrapper.

## 1.7.7

- Present native compilation progress in a frontmost overlay attached directly to the active export dialog viewport, keeping it visible above Godot's modal export interface on every desktop host.
- Move project scanning, parsing, semantic analysis, native generation, compilation, and linking onto one background build worker while the editor thread remains responsible only for window events, rendering, and export coordination.
- Snapshot third-party GDExtension ClassDB contracts before starting the worker and transfer progress through a mutex-protected mailbox, preventing background work from touching the live editor scene tree or UI.
- Compile compatible project GDScript only when an AOT export starts; ordinary editing, importing, and in-editor execution continue to use the original scripts without producing or loading a customer development library.
- Build exactly one selected Debug or Release target per export, removing the editor/development prebuild, generated customer CMake project, secondary runtime descriptor, and stale-library hot-reload chain.
- Unify every generated script under the attached-behavior backend so the original Godot or third-party GDExtension Node/Resource remains the real object owner while generated C++ supplies its ScriptInstance behavior.
- Add an export-scoped metadata bridge through `ScriptLanguageExtension`/`ScriptExtension`, including external scripts and embedded `.tres`/`.tscn` subresources, so scene conversion and ClassDB validation no longer require a host project library.
- Generate declaration-local export reflection directly from the compiler's project semantic graph, including inherited script identity, stored-property usage, method arguments, signals, and cache hits; export no longer loads every customer `.gd`, runs static initialization, or retains cyclic GDScript resources.
- Serialize only the source SceneState/Resource fields explicitly copied into each metadata-only ScriptInstance, leaving untouched defaults to the target C++ behavior constructor and preventing empty typed containers or `nil` values from overriding AOT defaults.
- Route cross-script fields, methods, Autoloads, `is`/`as`, internal classes, RefCounted objects, and explicit `self` through one attached-script identity and dispatch contract; preserve direct static type access without converting C++ class names into Variants.
- Dispatch ABI-compatible attached-script `self` calls through the generated C++ virtual hierarchy, while retaining ScriptLanguage dynamic dispatch for peer objects and ABI-changing overrides.
- Keep inherited ClassDB calls on the provider-owned Godot object while routing generated script dispatch to its attached behavior, preserving native GDExtension methods that call back into customer script overrides.
- Preserve semantic value types when attached or dynamic property reads cross the Variant boundary, including typed dictionaries and cross-script accessors.
- Ship one optimized `template_release` godot-cpp archive per customer target SDK and use it for both Debug and Release exports; keep the compiler's editor binding private to the prebuilt plugin instead of distributing a second customer static library.
- Replace the 16 per-version and complete archives with exactly three cross-version desktop packages: `gdpp-mac.zip`, `gdpp-linux.zip`, and `gdpp-win.zip`. Each package carries its own compiler/fallback and host Release SDKs for Godot 4.4–4.7; all three include Android and Web Release SDKs, while only macOS includes iOS.
- Stage host compiler/fallback binaries through an explicit runtime allowlist so Windows release packages exclude MSVC `.lib`/`.exp` import artifacts.
- Compile every generated customer translation unit once for the selected export target and keep Debug/Release object caches isolated while sharing deterministic frontend and generated-source state.
- Preserve GDScript's `Dictionary.key` read and write semantics when the receiver crosses a `Variant` boundary, including JSON/HTTP response dictionaries and nested compound assignments; asynchronous authentication and networking callbacks can now consume response fields without silently receiving null values.
- Preserve shared storage semantics for all ten `PackedArray` types across local aliases, fields, parameters, returns, default arguments, `Callable`, lambdas, signals, and dynamic calls while retaining independent explicit copies and parameter rebinding.
- Bind public script methods through the Variant ABI so godot-cpp ptrcalls cannot trigger copy-on-write separation at GDExtension boundaries; route subscripts, iteration, method calls, and engine API conversions through the same shared-storage contract.
- Carry resolved parameter contracts from semantic analysis through HIR and project interfaces, ensuring same-script, inherited, nested-class, and cross-script calls materialize arguments according to the callee's actual native ABI instead of the caller's inferred expression type.
- Centralize every generated native-to-Variant boundary behind an overload-independent runtime adapter, including arrays, dictionaries, dynamic calls, signals, callables, match bindings, external providers, utility functions, and engine varargs.
- Make native `PackedArray` conversion explicit and adapt fixed engine arguments separately, preventing MSVC overload ambiguity without reintroducing value copies or weakening the reflected Variant ABI.
- Advance the packaged SDK to schema 11 and runtime ABI 10, requiring the single-binding export contract and validating the attached/reference-semantics runtime throughout desktop, Android, iOS, and Web manifests.
- Coordinate compiler PDB writes during parallel MSVC SDK builds so large godot-cpp targets cannot fail nondeterministically on database contention; customer exports remain serialized per translation unit.
- Make Visual Studio multi-config builds select Release SDKs explicitly, record the actual editor optimization, and place plugin DLLs directly in the install-ready `binary` directory.
- Reduce the `GDPP AOT Build` overlay to a title and current-task row, and append a live per-file counter while compiling project sources.
- Submit progress geometry and changing task text directly to the rendering server, synchronizing each forced presentation so Windows updates both without requiring window movement.
- Isolate build-specific compiler descriptors under Godot's non-scanned `.godot` metadata directory and purge the retired add-on-local descriptor family during configuration, preventing duplicate ClassDB registration and broken Android scene conversion in reused build trees.

## 1.7.6

- Present native compilation progress in an exclusive transient window bound to the currently focused export dialog, keeping it visible above Godot's modal export interface on every desktop host.
- Replace the column-based overlay and per-profile resets with one continuous progress bar that remains monotonic across the complete AOT operation.
- Divide the bar equally across development and distribution builds, then equally across scan, parse, analysis, script precompilation, native-file generation, project compilation, and linking; per-file callbacks subdivide each applicable phase.
- Show live file counters without exposing backend translation terminology, and use concise AOT, Debug, and Release build labels throughout the window.

## 1.7.5

- Correct Windows MSVC environment bootstrapping so the raw `cmd.exe /c` payload reaches `vcvars64.bat` and `cl.exe` without C-runtime quote escaping corrupting the nested command.
- Execute generated translation-unit compilation and linking strictly one command at a time, preventing export-time process bursts and avoiding toolchain memory contention on large projects.
- Keep the Godot editor responsive while each serialized native command runs and advance compilation progress after every completed translation unit.
- Capture hidden toolchain stdout and stderr on Windows, macOS, and Linux, preserve the failing file, phase, and exit code, and surface the bounded original diagnostics in the Godot export result.

## 1.7.4

- Include the native-build progress overlay in every commercial plugin archive so fresh installations can load the editor plugin before export.
- Fail release assembly when the progress overlay is absent from either the staged add-on or the final deterministic ZIP.
- Use the optimized `template_release` godot-cpp binding for both Debug and Release game exports while retaining the separate `editor` binding required by the Godot editor process.
- Remove every `template_debug` archive from host, Android, iOS, and Web SDK packages; each platform/thread ABI now carries one distribution binding instead of duplicating hundreds of megabytes of debug objects.
- Preserve GDScript Debug-export `assert()` evaluation through a GDPP-owned compile definition, independently of godot-cpp's binding target and native optimization profile.
- Optimize, dead-strip, and hide symbols in Debug-export project libraries with the same commercial native pipeline used for Release exports.
- Advance the packaged SDK to schema 9, declaring the distribution binding and optimization profile explicitly and rejecting stale, debug-bound, or mixed SDK installations before compilation.
- Add `gdpp-4.4.zip` through `gdpp-4.7.zip` as per-SDK-version complete plugin packages, each combining the macOS arm64, Linux x86_64, and Windows x86_64 editor binaries and desktop SDKs with Android arm64, iOS device/Universal Simulator, and Web threads/nothreads export SDKs.
- Auto-select the current desktop host SDK from the complete package's platform-scoped layout while retaining compatibility with the smaller single-host packages.
- Make complete-package assembly reproducible and fail closed on missing hosts or targets, static add-on conflicts, runtime ABI/hash disagreements, mixed SDK versions, nested archives, generated products, or forbidden Debug bindings.
- Remove the standalone third-party notices aggregation from the installed add-on and every release archive.

## 1.7.3

- Run Windows export compiler, environment-bootstrap, and linker subprocesses without creating visible console windows.
- Preserve bounded parallel compilation and stage-ordered linking through an explicit direct-build API; the Godot export path no longer carries a configurable CMake-generation switch.
- Show a dedicated, continuous native-build progress overlay before Godot packaging, with real per-file progress for project scanning, GDScript parsing, semantic analysis, GDScript-to-C++ translation, C++ compilation, and native linking.
- Keep frontend, compile, and link progress stage-ordered and monotonic across development and distribution builds, then remove the overlay before Godot's packaging progress begins.

## 1.7.2

- Completed lossless third-party GDExtension reflection for Variant, typed Array/Dictionary, Signal, object classes, engine-typed Dictionary contracts, and encoded container metadata, preserving provider APIs without source changes.
- Reworked internal-class overrides around the emitted C++ ABI: compatible overrides now use the native virtual slot, incompatible GDScript contracts receive private implementation symbols, and Variant receivers retain dynamic dispatch.
- Propagated internal method contracts through receiver, `super`, default-argument, variadic, and coroutine calls so inherited behavior no longer falls back to name-only or semantic-type guesses.
- Canonicalized nested enum identities across generated declarations, parameters, returns, containers, and cross-script references, eliminating invalid C++ type names in large multi-script builds.
- Made attached-script descriptor replacement compare complete property, method, signal, constant, default-value, metadata, and RPC identities; hardened Callable copy/move assignment against self-aliasing.
- Advanced the packaged SDK to schema 8, requiring the MSVC frontend, a declared 19.x toolset, static CRT compatibility, and matching manifest metadata before Windows commands are created.
- Centralized the delivered target matrix and now reject unavailable Windows arm64, Linux arm64, and Android x86_64 exports before project generation; generated descriptors no longer advertise missing desktop ARM64 libraries.
- Kept the zero-source-change workflow export-only: ordinary editing and runtime continue to use the project's existing `.gd` files; extended or new `.gdpp` syntax is outside this release.

## 1.7.1

- Completed typed variadic functions, constructors, methods, lambdas, reflection metadata, call thunks, cache fingerprints, and attached-script dispatch across Godot 4.4–4.7 while keeping the grammar owned by GDPP rather than the host GDScript parser.
- Completed cross-script preload namespaces for root and nested classes, enums, constants, static fields and functions, typed resource references, casts, type tests, inheritance, and canonical inspector enum identities.
- Hardened cross-version native object lowering for RefCounted values, raw Object pointers, singleton wrappers, property getter return types, explicitly typed null branches, dynamic calls, and coroutine return ABIs.
- Made runtime/export discovery resolve script and scene Autoload UIDs, rewrite stripped Autoloads transactionally, reject editor-only runtime graphs, and isolate editor-only generated registrations.
- Eliminated native resource retention and self-assignment corruption by routing Dictionary, typed containers, String, StringName, NodePath, Array, PackedArray, Callable, and Signal storage through guarded assignment; attached-script descriptors now preserve the same rule.
- Centralized nested CMake toolchain propagation for compiler, toolchain file, sysroot, target triple, Apple deployment settings, MSVC CRT, RC/MT tools, generator platform, and toolset; independently built provider extensions now link the packaged SDK without CRT drift.
- Advanced the packaged SDK to schema 7 and runtime ABI 8, with fail-closed C++17, exception model, static MSVC CRT, Android `c++_shared`, source-integrity, platform, architecture, profile, and runtime validation.
- Admit only the exact Godot 4.4 parser diagnostics caused by newer GDPP-owned syntax while continuing to reject every unrelated import, export, runtime, and PCK diagnostic.

## 1.7.0

- Added zero-source-change AOT support for GDScript classes that extend types owned by an independent third-party GDExtension, without rebuilding, relinking, or modifying the provider plugin.
- Introduced an attached AOT backend built on `ScriptLanguageExtension`, `ScriptExtension`, and `ScriptInstance`: the provider continues to own the native Object while generated C++17 behavior supplies script fields, methods, properties, signals, notifications, and RPC metadata.
- Preserved script inheritance above an attached native root, including field initialization, `_init`, method/property dispatch, virtual callbacks, signal behavior, and inherited or overridden RPC configuration.
- Implemented exact external `super` calls through the provider's reflected MethodBind compatibility hash and the stable GDExtension ABI instead of unsafe cross-library C++ inheritance or guessed signatures.
- Extended ClassDB capture and `gdpp_bridge.json` validation with provider identity, exact callable metadata, load-order-independent provider resolution, cache invalidation, and fail-closed diagnostics when required reflection is unavailable.
- Converted scenes, standalone resources, embedded resources, and Autoloads to attached compiled scripts during binary-only export while retaining provider-owned native types and serialized state.
- Preserved third-party descriptors and target libraries byte for byte in exported games, and made attached startup safe for either provider-first or project-first registry order without requiring customer project or vendor source changes.
- Hardened macOS Universal 2 export with transactional provider-descriptor normalization, per-slice Mach-O validation, crash recovery, and restoration of every source descriptor after export.
- Advanced the packaged SDK to schema 6 and runtime ABI 7, hashing and validating all attached runtime headers and sources for host, desktop, mobile, and Web targets.

## 1.6.0

- Completed the Godot Variant type domain, nullability model, and zero-value truthiness semantics across semantic analysis, typed HIR, generated C++17, and the native runtime.
- Centralized strict assignment, explicit conversion, analyzer compatibility, runtime constructibility, and native storage rules for numeric, string, built-in value, packed array, object, Ref, RID, and container types.
- Added parameterized `as` targets for `Array[T]` and `Dictionary[K,V]`, qualified cross-script enum cast targets, strict constant-cast validation, guarded runtime casts, and compile-time rejection of deterministic String/RID paths that native storage cannot preserve.
- Enforced invariant typed-container storage with exact runtime element, key, value, object-class, and script metadata instead of relying on godot-cpp converting constructors, while preserving GDScript's analyzer acceptance of untyped and packed container boundaries.
- Added stable diagnostics for impossible constant casts, direct invariant typed-container violations, deterministic runtime cast failures, and values such as Object-backed RID expressions that native GDExtension storage cannot preserve.
- Keep nested warning-control scopes distinct and normalize the analyzer to the pinned formatter, preserving warning-clean generated code across Clang, GCC, and MSVC.
- Preserved abstract native script contracts during binary-only export by carrying compiler metadata into ClassDB validation and patching the required runtime registration API into every host, Android, Web, and iOS SDK.
- Advanced the native runtime ABI to 5 for the new guarded conversion and typed-storage contract.

## 1.5.0

- Added end-to-end flow-sensitive typing for `is`/`is not`, null and truthiness checks, short-circuit expressions, `if`/`while`, conditional expressions, post-dominating guards, structural `match`, and guarded bindings.
- Introduced stable symbol identities and a branch-state lattice with conservative joins, reassignment invalidation, callable isolation, and bounded analysis of large logical chains.
- Preserved both effective and storage types plus non-null proofs in typed HIR, allowing the C++ backend to specialize Variant-backed reads without changing their native storage ABI.
- Added source-located null/released-object guards for statically resolved method calls and property reads instead of permitting unchecked native dereferences.
- Completed default-argument lowering and Callable metadata, including omitted-argument dispatch, override compatibility checks, callable arity validation, and exact Godot virtual ABI adapters.
- Implemented the constant and runtime contracts for GDScript language utilities, including `assert`, `is_instance_of`, `type_exists`, `convert`, `str_to_var`, and `var_to_str`.
- Made unresolved values, types, and annotation constants fail closed, and constant-folded exported property hint/usage metadata while preserving Godot's script-variable identity.
- Enforced static-context ownership across fields, methods, accessors, `super`, node shorthand, and nested lambdas, while retaining valid static and inner-class access.
- Sequenced every eager binary operand explicitly so generated C++17 preserves GDScript's left-to-right side-effect order across native, Variant, membership, power, and comparison expressions.

## 1.4.0

- Unified frontend constant evaluation, HIR optimization, generated typed C++, and dynamic runtime integer operations behind one portable 64-bit contract.
- Defined deterministic wrapping, normalized shifts, signed division/modulo boundaries, compound assignments, and overflow-safe native range termination without C++ undefined behavior.
- Advanced the packaged SDK to schema 5 and runtime ABI 4, hashing and validating the shared integer-semantics header for every host and export target.

## 1.3.0

- Completed native `for` loop support for floating-point counts, Vector2/Vector2i and Vector3/Vector3i ranges, including direction, step, and boundary behavior matching Godot.
- Added statically typed object iterator support through Godot's `_iter_init`, `_iter_next`, and `_iter_get` protocol, with compile-time validation and typed results.
- Strengthened typed iterator safety by contextually typing array and dictionary literals, precisely diagnosing invalid elements, keys, and iterator declarations.

## 1.2.0

- Added end-to-end native support for typed arrays and dictionaries, preserving their element, key, and value contracts through semantic analysis, cross-script interfaces, and generated C++.
- Enforced typed container safety across assignments, arguments, returns, exported properties, nested containers, and project dependency boundaries.
- Improved generated-code performance by constructing typed container literals directly in their native representation and eliminating redundant wrapper copies.

## 1.1.0

- Significantly improved compatibility with the latest GDScript syntax, including strings, numbers, Unicode identifiers, local constants, trailing commas, and lambdas.
- Added complete support for array, dictionary, nested, and rest `match` patterns across type checking, optimization, and native code generation.
- Completed `await` and coroutine compilation with return values, immediate completion, concurrent calls, and dynamic dispatch across inheritance hierarchies.
- Strengthened multi-script project compilation with improved type and call-graph analysis, precise dependency invalidation, and incremental rebuilding.
- Hardened compiler safety and diagnostics with Unicode keyword spoofing protection, malformed-input handling, and configurable frontend resource limits.
- Improved cross-platform export compatibility by inheriting official Godot template settings.

## 1.0.0

- Initial release, featuring a GDScript AOT compiler and Godot editor plugin.
