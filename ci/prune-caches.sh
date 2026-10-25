#!/usr/bin/env bash
# Deletes the old caches of this repository, keeping only the newest of each kind (the dependency
# cache "ps4-deps-..." and the compiler cache "ccache-..."), so they do not pile up in Settings >
# Caches. Needs GH_TOKEN and "actions: write". Failing to delete is never an error for the build.
set -uo pipefail
for prefix in ps4-deps- ccache-; do
    ids="$(gh cache list --key "$prefix" --sort created_at --order desc --limit 100 --json id --jq '.[1:][].id' 2>/dev/null)"
    for id in $ids; do
        gh cache delete "$id" >/dev/null 2>&1 && echo "deleted an old $prefix cache ($id)"
    done
done
exit 0
