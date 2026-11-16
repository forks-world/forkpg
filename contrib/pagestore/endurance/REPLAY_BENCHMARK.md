# Recovery replay benchmark

Use a disposable snapshot: opening a store can reconcile interrupted writes and
modify its files. Never point the benchmark at a live or sole copy of a store.
Create a consistent snapshot using a method appropriate for the storage provider.

For the immutable WAL portion, build and run:

```sh
cc -O2 -pthread -Icontrib/pagestore \
  contrib/pagestore/endurance/benchmark_wal_recovery.c \
  contrib/pagestore/pagestore_wal_store.c \
  contrib/pagestore/pagestore_wal_segment.c \
  contrib/pagestore/pagestore_fault.c -o /tmp/benchmark_wal_recovery
/tmp/benchmark_wal_recovery /path/to/disposable/wal_segments_0 1 1048576
```

The reported elapsed time includes opening and verifying every retained segment;
the segment count confirms that runs exercised the same amount of data. Record
the source revision, compiler flags, CPU load, cache conditions and storage
provider. Compare repeated alternating runs; do not infer a speedup from one run.

For full daemon recovery, set `PAGESTORE_BENCH_RECOVERY=1` and run the daemon
against a disposable store. The `BENCH` lines report layer index collection,
payload verification, ordering and page replay separately, plus input counts.
The daemon's ready message marks completion of all startup stages, including
WAL segment recovery. Some stores contain absolute layer paths: reproduce their
original paths in an isolated namespace or use a snapshot whose paths are valid.
