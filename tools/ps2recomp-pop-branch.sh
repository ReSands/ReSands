#!/bin/zsh
# Builds the local "pop" branch of ~/tools/PS2Recomp: upstream main plus open
# upstream PRs we reviewed (11 Oct 2026, see CLAUDE.md "PS2Recomp PRs").
# One merge commit per PR; a PR that conflicts is skipped and reported.
# Usage: tools/ps2recomp-pop-branch.sh        (recreates the branch from origin/main)
set -euo pipefail

REPO=~/tools/PS2Recomp
# Order matters only where PRs touch the same code (#277 before anything float-related).
PRS=(268 277 215 195 235 239 241 266 267 270 279)

cd $REPO
git fetch -q origin main '+refs/pull/*/head:refs/remotes/pr/*'
# Our earlier local vcallmsr edit is superseded by #268.
git checkout -q -- ps2xRecomp/src/lib/vu_translation_helpers.cpp
git checkout -q -B pop origin/main

for n in $PRS; do
    if git merge -q --no-ff --no-edit -m "Merge upstream PR #$n" pr/$n >/dev/null 2>&1; then
        echo "#$n merged"
    else
        echo "#$n CONFLICT, skipped: $(git diff --name-only --diff-filter=U | tr '\n' ' ')"
        git merge --abort
    fi
done
echo "pop = $(git rev-parse --short HEAD), $(git rev-list --count origin/main..pop) commits on top of origin/main"
