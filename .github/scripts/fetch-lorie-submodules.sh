#!/usr/bin/env bash
# Fetches the native X server dependencies of :lorie (Termux:X11 core) at the exact commits
# pinned in lorie/submodules.tsv. Run from the repository root.
set -euo pipefail

while IFS=$'\t' read -r name url sha path; do
    [ -z "$name" ] && continue
    echo "::group::$name ($sha)"
    rm -rf "$path"
    mkdir -p "$path"
    (
        cd "$path"
        git init -q
        git remote add origin "$url"
        if ! git fetch -q --depth 1 origin "$sha"; then
            # Some hosts refuse fetching a bare SHA; fall back to a full fetch.
            git fetch -q origin
        fi
        git checkout -q "$sha"
    )
    echo "::endgroup::"
done < lorie/submodules.tsv
