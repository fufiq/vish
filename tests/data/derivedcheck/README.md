# RA-4a (mine)

`vish derivedcheck`: 20 cycles, two publications, a rollback to the previous accepted event,
a change of the source input between the snapshot and the publication, a refusal of a stale rollback;
refusals without checks and with a counterexample; a generator version change. The journal fold from scratch
compares the statuses, the reasons, the active/working records and the counter of stale publications.
`main.vish` runs the same oracle with the module next to it; `mutants.sh` creates six independent
copies of the module. A mutation succeeds only as `oracle return=1`; a parse error does not count.

## Known limitations

This is a data model, not a generator and not an executor of machine code (RA-4b is ahead).
One store has one active variant; the records and the journal are in memory, without disk persistence.
The read-set holds the slot indexes and the RA-2 revisions: it belongs to one reactive_db;
moving between databases is forbidden by contract. `derived_reads` includes the root query's revision
and its read-set; the source must be an input/query of this snapshot. The source identity,
the hash, the parameters, the check numbers and the costs are supplied by the calling code. The hash is not a cryptographic guarantee.
The check numbers are not certified by the model; zero mismatches mean that no counterexamples
were found on the stated corpus. The budget and measured-gain checks are RA-4b.
Database changes during publication are not allowed (a single-threaded interpreter, no callbacks).
The journal is trusted, append-only by the API; a direct change of public fields is not supported.
The last working record is kept even after going stale; the active one is dropped when the
generator changes. An edit of the input alone does not drop an already active variant: the check before execution
and the reactive subscription are an RA-4b integration task. A rollback re-checks the dependencies and the version.
The fold oracle is independent of the transition functions, but trusts the events, including the observed revisions
of the publication (it recomputes the status decision itself); the fixed expectations additionally check the refusal of a stale publication.
Random sequences, concurrent publication and recovery after a crash are not covered.
