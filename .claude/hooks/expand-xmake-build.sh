#!/usr/bin/env bash
# .claude/hooks/expand-xmake-build.sh
#
# Claude Code PreToolUse hook for Bash. `xmake build` only accepts one target
# per invocation, so we rewrite `xmake build a b ...` into chained
# single-target builds.
#
# Composed commands: we split on top-level `&&` and `;` (respecting single /
# double quotes and backslash escapes) and expand each segment independently.
# A segment is rewritten only when it is a pure `xmake build <t1> <t2> ...`
# (≥ 2 positional targets, no flags, no pipes / redirects / parens /
# background). Any other segment passes through verbatim, so e.g.
# `xmake build a b 2>&1 | tail -5 && echo done` is left alone (the first
# segment has `|` / `>` — operator stays in control of redirect / pipe scope).
#
# Whole-command bail-outs: we cannot reason about backticks or `$(...)`
# without a real shell tokenizer, so the command passes through unchanged if
# either appears.
#
# Input: hook JSON on stdin with `.tool_input.command`.
# Output (only when at least one segment was expanded): PreToolUse JSON on
# stdout with `hookSpecificOutput.updatedInput.command` set to the rewrite.

set -euo pipefail

input=$(cat)
command=$(printf '%s' "$input" | jq -r '.tool_input.command // empty')

if [[ -z "$command" ]]; then
  exit 0
fi

# Subshell / command-substitution: we can't track boundaries through the
# split-on-&&/; pass, so bail entirely.
if [[ "$command" == *'`'* ]] || [[ "$command" == *'$('* ]]; then
  exit 0
fi

# Split $1 on top-level `&&` and `;`, respecting single / double quotes and
# backslash escapes. Populates the `segments` and `seps` arrays (len(seps) =
# len(segments) - 1; seps[i] is the operator between segments[i] and
# segments[i+1]).
split_top_level() {
  local s="$1"
  local n=${#s}
  local i=0
  local in_single=0 in_double=0
  local cur=""
  segments=()
  seps=()
  while ((i < n)); do
    local c="${s:i:1}"
    if ((in_single)); then
      cur+="$c"
      [[ "$c" == "'" ]] && in_single=0
      i=$((i + 1))
      continue
    fi
    if ((in_double)); then
      if [[ "$c" == "\\" ]] && ((i + 1 < n)); then
        cur+="${s:i:2}"
        i=$((i + 2))
        continue
      fi
      cur+="$c"
      [[ "$c" == '"' ]] && in_double=0
      i=$((i + 1))
      continue
    fi
    case "$c" in
    "'")
      in_single=1
      cur+="$c"
      i=$((i + 1))
      ;;
    '"')
      in_double=1
      cur+="$c"
      i=$((i + 1))
      ;;
    "\\")
      if ((i + 1 < n)); then
        cur+="${s:i:2}"
        i=$((i + 2))
      else
        cur+="$c"
        i=$((i + 1))
      fi
      ;;
    '&')
      if [[ "${s:i+1:1}" == '&' ]]; then
        segments+=("$cur")
        seps+=("&&")
        cur=""
        i=$((i + 2))
      else
        # Bare `&` (background or stray). Keep verbatim — the per-segment
        # guard will refuse to expand any segment containing it.
        cur+="$c"
        i=$((i + 1))
      fi
      ;;
    ';')
      segments+=("$cur")
      seps+=(";")
      cur=""
      i=$((i + 1))
      ;;
    *)
      cur+="$c"
      i=$((i + 1))
      ;;
    esac
  done
  segments+=("$cur")
}

# Print the (possibly rewritten) form of $1. When unchanged, prints the
# segment verbatim; when rewritten, prints the chained form without the
# segment's original surrounding whitespace (bash doesn't care, and it keeps
# the diff clean).
expand_segment() {
  local seg="$1"
  local trimmed="${seg#"${seg%%[![:space:]]*}"}"
  trimmed="${trimmed%"${trimmed##*[![:space:]]}"}"
  if [[ -z "$trimmed" ]]; then
    printf '%s' "$seg"
    return
  fi
  # Any shell metachar means this segment isn't a pure build — leave it.
  if [[ "$trimmed" =~ [\&\|\>\<\(\)] ]]; then
    printf '%s' "$seg"
    return
  fi
  local body="$trimmed"
  if [[ "$body" != "xmake build "* ]]; then
    printf '%s' "$seg"
    return
  fi
  local args_str="${body#xmake build }"
  local tokens=()
  read -ra tokens <<<"$args_str"
  for tok in "${tokens[@]}"; do
    if [[ "$tok" == -* ]]; then
      printf '%s' "$seg"
      return
    fi
  done
  if ((${#tokens[@]} < 2)); then
    printf '%s' "$seg"
    return
  fi
  local chained=""
  for tgt in "${tokens[@]}"; do
    local segment_cmd="xmake build ${tgt}"
    if [[ -z "$chained" ]]; then
      chained="$segment_cmd"
    else
      chained="$chained && $segment_cmd"
    fi
  done
  printf '%s' "$chained"
}

split_top_level "$command"

any_expanded=0
out=""
for idx in "${!segments[@]}"; do
  seg="${segments[$idx]}"
  exp=$(expand_segment "$seg")
  if [[ "$exp" != "$seg" ]]; then
    any_expanded=1
  fi
  if ((idx == 0)); then
    out="$exp"
  else
    sep="${seps[$((idx - 1))]}"
    out="$out $sep $exp"
  fi
done

if ((!any_expanded)); then
  exit 0
fi

jq -n --arg cmd "$out" --arg orig "$command" '{
  hookSpecificOutput: {
    hookEventName: "PreToolUse",
    updatedInput: { command: $cmd },
    additionalContext: "xmake build only accepts one target per invocation; multi-target segments were auto-expanded into chained single-target builds."
  },
  systemMessage: ("xmake build auto-expanded: " + $orig + " -> " + $cmd)
}'
