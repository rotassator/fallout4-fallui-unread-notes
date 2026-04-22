#!/usr/bin/env bash
# PostToolUse hook: after a git command, check whether submodule SHAs changed
# since the last security review. If yes, output additionalContext asking the
# model to run /security-review on the changed dependency code.
#
# Stdin: { "tool_name": "Bash", "tool_input": { "command": "..." }, ... }
# Exit 0 on either no-op or successful gate notification (non-blocking).
#
# Uses Python (not jq) for JSON parsing because jq is not bundled with Git Bash.

set -u

# Capture stdin once, then feed it to python for JSON extraction
PAYLOAD=$(cat)
CMD=$(printf '%s' "${PAYLOAD}" | python -c \
    "import sys,json; print(json.load(sys.stdin).get('tool_input',{}).get('command',''))" \
    2>/dev/null)

# Narrow further than the if filter (which is just "Bash(git *)") — only
# commands that can move submodule pointers. Avoids work on git status etc.
case "$CMD" in
    *"git submodule update"*|\
    *"git pull"*|\
    *"git checkout"*|\
    *"git clone"*|\
    *"git submodule add"*|\
    *"git submodule sync"*)
        ;;
    *)
        exit 0
        ;;
esac

# Derive project root from script location: <root>/.claude/hooks/<this>
PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STATE_FILE="${PROJECT_ROOT}/.claude/.submodule-review-state"

cd "${PROJECT_ROOT}" 2>/dev/null || exit 0

# git submodule status output: " <SHA> <path> (<describe>)"
# Normalise to "path:SHA" lines, sorted, so the comparison is order-independent.
CURRENT=$(git submodule status --recursive 2>/dev/null \
    | awk '{gsub(/^[+\- ]/, "", $1); print $2 ":" $1}' \
    | sort)

# No submodules → nothing to gate
[ -z "${CURRENT}" ] && exit 0

PREVIOUS=$(cat "${STATE_FILE}" 2>/dev/null || true)

if [ "${CURRENT}" = "${PREVIOUS}" ]; then
    exit 0
fi

# Submodule pointers moved (or first run) — record new state and notify
mkdir -p "$(dirname "${STATE_FILE}")"
printf '%s\n' "${CURRENT}" > "${STATE_FILE}"

# Build a human-readable diff of what changed (path-by-path)
DIFF_SUMMARY=$(diff <(printf '%s\n' "${PREVIOUS}") <(printf '%s\n' "${CURRENT}") \
    | grep -E '^[<>]' \
    | head -10 \
    | sed 's/^/    /')

# Build the JSON output via python so multiline content is properly escaped.
export DIFF_SUMMARY
python <<'PYEOF'
import json, os, sys

ctx = (
    "Submodule dependency update detected. The recorded SHAs in "
    ".claude/.submodule-review-state did not match the post-command state. "
    "Changes:\n"
    + os.environ.get("DIFF_SUMMARY", "")
    + "\n\nBefore relying on the updated dependency code, run the "
    "/security-review skill on the changed submodule trees: "
    "lib/commonlibf4/ and lib/commonlibf4/lib/commonlib-shared/. "
    "Focus on diffs since the previous SHA: review for suspicious patterns "
    "(network calls, embedded credentials, unusual obfuscation, post-install "
    "scripts, RCE vectors). The state file has already been updated to the "
    "current SHAs, so re-running this hook for the same change will be a no-op."
)

print(json.dumps({
    "systemMessage": "Submodule pointers changed — security review recommended on lib/commonlibf4 (and any nested submodules) before continuing.",
    "hookSpecificOutput": {
        "hookEventName": "PostToolUse",
        "additionalContext": ctx,
    },
}))
PYEOF
