The six Eden files that `patches/eden-jit/0002-JIT-direct-page-table-copy.py` edits, exactly as the build has them: Eden commit
`5f142c7926d0c7fcbbd0ce30794d72f638a43b2a` (the one `scripts/fetch-eden.sh` pins) with the 30 patches of `patches/eden/` and part 1 of the JIT pack
applied (the order the build does it in). `tests/jit_direct_test.sh` runs the script on them as well as on the Eden master excerpts in `../fixtures/`:
0.9.3 matched master only, and so did not fit the Eden the build really uses (`constexpr` instead of `inline` on `Store` / `MarkRasterizerCached`,
and the stride shift in the A64 lookup is not a branch). Eden's licence (GPL-2.0-or-later) applies to these copies.
