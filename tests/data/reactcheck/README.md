RA-2 (mine, 2026-11-16)

The API is documented in lib/reactive.vish. Values may have different types;
the comparator passed to new_db must support all the types in use.
For code, compare parser.dump_code, as in modcheck_demo.vish.
Queries are pure; set_input/query_define and watch registration during computations
or notifications are not supported. Exceptions clear the computation stack;
batch is not a transaction with a rollback. The counters are cumulative.

Run: out/vish reactcheck [seed] [steps]. Examples with modules next to
the program: sh tests/data/reactcheck/run.sh out/vish.

Basis: the author's earlier remark, confirmed:
"then only what is inside the brackets is parsed". The original is checked.
The universal engine, push/pull, the order of subscriptions and the API are mine per the RA-2 brief.
The 2020-07-24 remark is probable; it is not attributed to the author.

Vish specifics: str has no < operator (we compare bytes);
use modules are not transitive; the closure specification is copied into a local
value before saving; there is no general array.slice (we copy a prefix).

Measurements, 300 packets, 12 queries:
seed 1: 2780 / 7812; seed 17: 2720 / 6140; seed 91: 2599 / 7372.
Push and pull have the same number of computations; 0 violations.
The parser demo: initially=5 edited=1 cached=4.
