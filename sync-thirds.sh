#!/usr/bin/env bash
# sync-thirds.sh - bring the thirds/ submodules (zorm, mcp-cpp) to their latest
# remote master and record the new revisions in this repository.
#
# Workflow for changing zorm or mcp-cpp:
#   1. cd thirds/zorm            (or thirds/mcp-cpp) - it is a full checkout
#      ... edit, git add, git commit, git push
#   2. ./sync-thirds.sh          - pulls the new master into the submodule and
#                                  commits the updated pointer here
#
# Note: committing inside the submodule alone leaves this repository pointing
# at the old revision; a later `git submodule update` would then move the
# submodule back. That is what step 2 fixes.
set -euo pipefail
cd "$(dirname "$0")"

SUBMODULES=(thirds/zorm thirds/mcp-cpp)

git submodule update --remote --merge "${SUBMODULES[@]}"

if git diff --quiet -- "${SUBMODULES[@]}"; then
  echo "[sync-thirds] thirds already at the recorded revisions"
  exit 0
fi

git add "${SUBMODULES[@]}"
git commit -m "chore: update thirds submodules to latest master"
echo "[sync-thirds] recorded new submodule revisions"
