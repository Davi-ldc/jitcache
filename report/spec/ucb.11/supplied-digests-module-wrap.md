mechanical

Requirement. SPEC-ucb.md section 13.3, `supplied-digests.js`: "a CommonJS module the app loads after overriding `Module.wrap` imports no body captured under the other wrapper, whichever run overrode it, and with strict on raises no invalid material". Section 7.2.6 states the matching change at the two sites that "replace `source.source_code` with the text of an overridden module wrapper".

Why the code cannot meet it as written. In Bun, overriding `Module.wrap` changes nothing about how a CommonJS module loads. `Module.wrap` is a host function, `jsFunctionWrap`, that only returns wrapped text, and no loader calls it. The override that `JSCommonJSModule::evaluate` and `createCommonJSModule` apply is `Module.wrapper`: its custom setter, `setNodeModuleWrapper`, and the array its getter returns both reach `setModuleWrapper`, which sets `hasOverriddenModuleWrapper` with the new start and end. An app that only reassigned `Module.wrap` would load its module under Bun's default wrapper in both runs and reach neither site.

What the code does instead. Sequences 4 and 5 of `JSTests/jitcache/ucb/supplied-digests.js` build the app with `--format=cjs`. A `--compile` build without `--bytecode` is an ES build, whose embedded modules the module loader evaluates without any wrapper. The CommonJS build keeps the embedded module CommonJS, so `require` evaluates it through `JSCommonJSModule::evaluate`. Before that `require`, `JSTests/jitcache/ucb/resources/supplied-digests-app.js` assigns `Module.wrapper`, in the Producer (4) or in the Consumer (5).

The wrapper has to call the bundled one. `evaluate` keeps the module's text from its first newline, which in this build ends the `// @bun @bun-cjs` comment, and drops its last four characters, which hold the `})` closing the bundled wrapper. Any wrapper therefore surrounds a bundled wrapper that is opened and never closed, with JITCache off as well. The app's wrapper closes that function and calls it with its own arguments, so the module runs under other text and keeps its exports.

The checks are the SPEC's, plus one on the site itself:
- in the overriding run, the module's root digest is computed from its text (`sourceDigests` grows by one over its load), which shows that `evaluate` cleared the recorded digest;
- in the Consumer, the module's function finds no body (it misses `NoBody` and imports nothing);
- cache activity stays on with strict on, and T7 reports no difference.

Nothing is asked of other tasks.

Evidence.
- `jsFunctionWrap`, `setNodeModuleWrapper`, `nodeModuleWrapper` and `setModuleWrapper` (`~/bun/src/jsc/modules/NodeModuleModule.cpp`).
- `JSCommonJSModule::evaluate` and `createCommonJSModule` (`~/bun/src/jsc/bindings/JSCommonJSModule.cpp`), which read `hasOverriddenModuleWrapper`, `m_moduleWrapperStart` and `m_moduleWrapperEnd`.
- `fetchCommonJSModule` (`~/bun/src/jsc/bindings/ModuleLoader.cpp`), which evaluates an embedded file through `JSCommonJSModule::evaluate` only when `isCommonJSModule` is set.
- The standalone module's `ResolvedSource` (`~/bun/src/runtime/jsc_hooks.rs`), whose `is_commonjs_module` is `file.module_format == ModuleFormat::Cjs`; `to_bytes` (`~/bun/src/standalone_graph/StandaloneModuleGraph.rs`), which takes `module_format` from the build's output format; and the CLI's `output_format` (`~/bun/src/runtime/cli/Arguments.rs`), which only `--bytecode` or `--format` changes from ES modules.
- `post_process_js_chunk` (`~/bun/src/bundler/linker_context/postProcessJSChunk.rs`), which opens a CommonJS chunk with `// @bun @bun-cjs` and the wrapper on the next line and closes it with `})`.
