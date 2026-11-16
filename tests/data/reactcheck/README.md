RA-2 (mine, 2026-11-16; fixes after the review - the same day, two cycles)

The API is documented in lib/reactive.vish (the header). Values may have different types;
the comparator passed to new_db must support all the types in use.
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

Run: out/vish reactcheck [seed] [steps]. The oracle: 12 queries (7 kinds: sum, minimum, concatenation,
a conditional read, a guard with an exception, catching a read error, an optional key), inputs, optional
keys, repeated query definitions, subscriptions; push and pull against a naive recursive recomputation,
a structural audit of the edges, subscription delivery. Kinds 7 and 8 are exceptions of DIFFERENT types with the same text
and a typed catch (`catch(str e)`); the oracle's output includes the exception type. The statistics check:
accesses == computations + hits + refusals, computations == the independent count of query function executions
(a counter in the lambdas themselves), cutoffs <= computations. Then the fixed cases (regressions of both review cycles:
B1/B2, M1, M2, depth; a typed catch, a caught cycle + an independent subscriber, a failed diamond of depth 20,
a depth error and set_max_depth).
Examples with modules next to the program and a mutation check of the oracle (8 mutations, all must be caught):
sh tests/data/reactcheck/run.sh out/vish.

Basis: the author's earlier remark, confirmed:
"then only what is inside the brackets is parsed". The original is checked.
The universal engine, push/pull, the order of subscriptions and the API are mine per the RA-2 brief.
The 2020-07-24 remark is probable; it is not attributed to the author.

Vish specifics: str has no < operator (we compare bytes); use modules are not transitive;
a named function of the program's module cannot be passed to reactive (isolation) - wrap it in a lambda;
the closure specification is copied into a local value before saving; there is no general array.slice
or pop; free names are looked up along the caller chain - deep recursion is quadratic.
