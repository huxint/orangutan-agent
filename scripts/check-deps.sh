#!/usr/bin/env bash
# scripts/check-deps.sh
#
# Walks xmake/targets.lua, extracts each oran-X library's add_deps() list,
# and validates the result against the layering documented in
# docs/design-docs/module-boundaries.md ("Dependency Direction"). A library
# may depend only on libraries strictly below its own layer; sibling deps
# within the same layer are rejected unless an explicit exception is wired
# in below.
#
# Exit codes:
#   0 — every oran-X target's deps are downward and known.
#   1 — at least one upward, sideways, or unknown dep was found; details
#       printed to stderr before exit.
#
# When a new oran-X library lands, register it in `LAYER` below in the
# same commit that adds the xmake target — otherwise the script will
# reject the new target as unknown.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
targets="${repo_root}/xmake/targets.lua"

if [[ ! -f "${targets}" ]]; then
  echo "check-deps: ${targets} not found" >&2
  exit 1
fi

# Layering, lowest (foundation) → highest (composition root). Matches the
# diagram in docs/design-docs/module-boundaries.md "Dependency Direction".
# `bootstrap` is the composition root — it sits above the interface layer
# because it wires the entire process together and is the only target the
# binary entry point reaches into.
declare -A LAYER=(
  [core]=0
  # platform
  [async]=1
  [http]=1
  [io]=1
  [storage]=1
  [config]=1
  [log]=1
  # composition utilities
  [prompt]=2
  [tool]=2
  [memory]=2
  [permission]=2
  [hook]=2
  [skill]=2
  [provider]=2
  # agent runtime
  [agent]=3
  [orchestration]=3
  [automation]=3
  # interface
  [cli]=4
  [web]=4
  [channel]=4
  # composition root
  [bootstrap]=5
)

# Human-readable layer names for the failure messages.
declare -A LAYER_NAME=(
  [0]="foundation"
  [1]="platform"
  [2]="composition"
  [3]="agent-runtime"
  [4]="interface"
  [5]="composition-root"
)

# Explicit sibling-dep allowlist. Same-layer deps are forbidden by
# module-boundaries.md "Dependency Direction" UNLESS the relationship is
# documented in docs/ARCHITECTURE.md's library inventory and added here.
# Keys are `<dependent>__<dep>` pairs (double underscore — single dash
# trips shfmt's arithmetic reformatter inside `[]`).
#
# Current exceptions (all from xmake/targets.lua + ARCHITECTURE.md):
#   io     -> async    : every io call hops onto the executor.
#   storage-> async    : Pool/Repository acquire writer/reader slots via the executor.
#   config -> storage  : typed permissions block reuses storage's migration shape.
#   tool   -> permission: dispatch consults RuleSet + AuditSink directly.
#   tool   -> hook     : dispatch publishes tool_before / tool_dispatched / tool_error / tool_after.
declare -A ALLOWED_SIBLING=(
  [io__async]=1
  [storage__async]=1
  [config__storage]=1
  [tool__permission]=1
  [tool__hook]=1
)

failed=0

# `oran_lib("name", { "oran-dep1", "oran-dep2" }, ...)` is the canonical
# shape — `xmake/targets.lua` declares every library through that helper.
# We tolerate the dep list being empty (`{}`) and we tolerate optional
# trailing arguments (private_packages, public_packages).
while IFS= read -r line; do
  if [[ "${line}" =~ ^[[:space:]]*oran_lib\(\"([a-z][a-z-]*)\"[[:space:]]*,[[:space:]]*\{([^}]*)\} ]]; then
    name="${BASH_REMATCH[1]}"
    deps_str="${BASH_REMATCH[2]}"

    if [[ -z "${LAYER[${name}]+x}" ]]; then
      echo "check-deps: target oran-${name} is not registered in scripts/check-deps.sh LAYER table" >&2
      failed=1
      continue
    fi
    self_layer="${LAYER[${name}]}"
    self_layer_name="${LAYER_NAME[${self_layer}]}"

    # Extract every quoted oran-X token from the dep list.
    while read -r dep; do
      [[ -z "${dep}" ]] && continue
      dep_name="${dep#oran-}"
      if [[ -z "${LAYER[${dep_name}]+x}" ]]; then
        echo "check-deps: oran-${name} declares dep on unknown library oran-${dep_name}" >&2
        failed=1
        continue
      fi
      dep_layer="${LAYER[${dep_name}]}"
      dep_layer_name="${LAYER_NAME[${dep_layer}]}"
      if ((dep_layer > self_layer)); then
        echo "check-deps: upward dep — oran-${name} (${self_layer_name}) may not depend on oran-${dep_name} (${dep_layer_name})" >&2
        failed=1
      elif ((dep_layer == self_layer)); then
        if [[ -z "${ALLOWED_SIBLING[${name}__${dep_name}]+x}" ]]; then
          echo "check-deps: undocumented sibling dep — oran-${name} -> oran-${dep_name} (both ${self_layer_name}); add an entry to ALLOWED_SIBLING in check-deps.sh and to docs/ARCHITECTURE.md if intentional" >&2
          failed=1
        fi
      fi
    done < <(echo "${deps_str}" | grep -oE '"oran-[a-z][a-z-]*"' | tr -d '"')
  fi
done <"${targets}"

if ((failed)); then
  exit 1
fi

echo "check-deps: dependency layering ok"
