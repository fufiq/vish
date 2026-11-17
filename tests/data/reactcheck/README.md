RA-2 (mine, 2026-11-16; fixes after the review - the same day, four cycles)

The API is documented in lib/reactive.vish (the header). Values may have different types;
the comparator passed to new_db must support all the types in use. The recommended comparator is
`(a,b) => deep_equal(a,b)` (STRUCTURAL equality: the type at each level and the content of arrays/tuples/struct, not text();
the Vish `==` on values of different types is a program error, not false). It also compares query exceptions
(early cutoff of errors): `array{1}` and `array{"1"}`, `array{"a, b"}` and `array{"a","b"}`, `1` and `1.0`, `true` and `"true"` are different.
For code, compare parser.dump_code, as in modcheck_demo.vish.
Queries are pure; set_input/query_define/watch/batch during the computation of a query or
notifications are an explicit refusal "reactive: change during ...". A query exception clears the stack; an error is
the same cacheable result as a value (read-set, revision, early cutoff: the same exception of the same type
does not change the revision). A result affected by a caught cycle or a depth refusal is "volatile":
stored, but not cached (it depends on the caller/on set_max_depth). A batch exception keeps
the changes and is passed without notifications. A handler exception does not interrupt delivery: all
handlers are called, the first exception is rethrown afterwards (the rest are dropped). The counters are cumulative.
The computation depth is limited (set_max_depth, 2000 by default): "reactive: computation depth > N".

The dependency model: a read-set [key, revision] in read order; freshness - by the revisions
of the dependencies in order, stopping at the first one that changed; a failed read is a dependency on the revision
of the stored error (a change of the exception - including the type with the same text - and the appearance/fix of a key
recompute the parent). Subscription delivery: the order is by read-set; an edge cycle does not interrupt delivery.

Run: out/vish reactcheck [seed] [steps]. The oracle: 12 queries (11 kinds: sum, minimum, concatenation,
a conditional read, a guard with an exception, catching a read error, an optional key, a "thrower", a typed
catch, a catch with a report, an array result), inputs, optional keys, repeated query definitions,
subscriptions; push and pull against a naive recursive recomputation, a structural audit of the edges, subscription delivery.
Kind 7 throws an exception of one of 30 kinds (numbers, strings, arrays, nested arrays, a struct, tuples, bool, f64,
an empty array/string; many pairs have the SAME text for different types/structure); kind 8 is a typed catch
(`catch(str e)`); kind 9 is a catch with a report (a hash of the exception's full structural record), kind 10 is an array result
(early cutoff of arrays). The oracle's output is the structural record `rx_ser` (independent of `deep_equal`).
Batches are flat and NESTED (steps modulo 3). The statistics check: accesses == computations + hits + refusals;
computations == the independent count of query function executions (a counter in the lambdas themselves); cutoffs == the independent exact
count of "the execution result matched the previous one by rx_ser" (before - only <= computations). Notifications: per batch
(including a nested one) a subscriber gets exactly one notification when its query does not fail and the value differs from
the last delivered one (an exact count at every step and for every database). Then the fixed cases (regressions of the three review
cycles: B1/B2, M1, M2, depth; a typed catch, a caught cycle + an independent subscriber, a failed diamond of depth 20,
a depth error and set_max_depth; H1 - the truth table of `deep_equal` over 22x22 kinds, all pairs of exceptions with one text
and neighbouring kinds through a query/catch/consumer in push and pull, the reviewer's regressions `array{1}`->`array{"1"}` and
`array{"a, b"}`->`array{"a","b"}`, early cutoff of arrays with exact numbers, nested batches).
Examples with modules next to the program and a mutation check of the oracle (17 mutations, all must be caught;
a-h are the earlier ones, i-l are the third-cycle reviewer's mutations 9-12, m-n are our own for `deep_equal`): sh tests/data/reactcheck/run.sh out/vish.

Basis: the author's earlier remark, confirmed:
"then only what is inside the brackets is parsed". The original is checked.
The universal engine, push/pull, the order of subscriptions and the API are mine per the RA-2 brief.
The 2020-07-24 remark is probable; it is not attributed to the author.

Vish specifics: str has no < operator (we compare bytes); use modules are not transitive;
a named function of the program's module cannot be passed to reactive (isolation) - wrap it in a lambda;
the closure specification is copied into a local value before saving; there is no general array.slice
or pop; free names are looked up along the caller chain - deep recursion is quadratic.

Cycle 4 (mine, 2026-11-17): eight new payload classes 22-29 - a signed zero,
a nested array and a struct with a zero, code, ref, fn, type and NaN. The consumer observes
the sign by dividing 1.0/e, the code through code_text, the function by a call, the type by name.
The rx_ser oracle observes fn by a call with 3 (only generated pure unary functions),
and the independent rx_cuttable forbids expecting a cutoff of opaque/NaN even with the same observation.
The fixed cases check values and exceptions, a direct deep_equal and the consumer's recomputation;
cycle4.vish holds separate explicit expectations for H1/H2 and checks that the process continues.
Mutations o/p/q (15-17) independently resolve the equality of fn/type/NaN.

## Known limitations

- The negative slots of absent keys accumulate without cleanup.
- deep_equal conservatively returns false for functions, type values, code/ref,
  expr and unsupported parameterized types; NaN is also always false.
  For f64 the bits are compared after the NaN refusal: +0.0 and -0.0 differ, including nested ones.
- The oracle does not prove the equivalence of arbitrary functions or code: the observations
  are limited to the generated classes. The rx_ser function is a helper of this corpus.
- The depth of deep_equal is not limited separately; the success of a comparison at 6000 levels
  is not confirmed (a timeout in the earlier review). This is not a safety guarantee for any depth.
- The full check.sh integration check and an independent review are done by the coordinator.
