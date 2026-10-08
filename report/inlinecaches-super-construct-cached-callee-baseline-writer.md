# inlinecaches.md: the baseline code now writes the `super_construct` cache

Affected file: `skills/reference/knowledge/common/inlinecaches.md`, the Table row for `m_cachedCallee` of `super_construct` and `super_construct_varargs`.

The row's "who writes it" cell says only the LLInt writes the cache, because the baseline code stores back the value it loaded instead of the `new.target`. The branch for both opcodes in `JIT::compileOpCall` now stores the `new.target`. It follows the LLInt's `op_super_construct` and `op_super_construct_varargs` handlers: an equal value leaves the cache as it is, an empty cache takes the `new.target`, a cache holding another cell takes the marker (`JSCell::seenMultipleCalleeObjects()`), the varargs form skips a `new.target` that is not a cell, and neither tier issues a write barrier. A `CodeBlock` born in baseline therefore fills the cache, and one that tiered up from the LLInt keeps updating it.

Proposed cell:

> the LLInt and the emitted baseline code (`JIT::compileOpCall`), with the same rule and no write barrier; GC clears it

The "reads at runtime" cell (LLInt and the emitted code) stays correct. No other reference doc describes the baseline store.
