#!/usr/bin/env bash
# Claude Code PreToolUse hook for Bash.
#
# xmake build accepts a single positional target. This hook rewrites pure
# multi-target build segments:
#
#   xmake build -r test-core test-tool
#
# into:
#
#   xmake build -r test-core && xmake build -r test-tool
#
# It intentionally leaves segments with pipes, redirects, parens, backgrounding,
# or command substitution untouched because changing those can alter shell scope.

set -euo pipefail

input=$(cat)
HOOK_INPUT="$input" python3 - "$@" <<'PY'
import json
import os
import shlex
import sys

OPTION_VALUE_FLAGS = {
    "-F",
    "--file",
    "-P",
    "--project",
    "--confirm",
    "-j",
    "--jobs",
    "--linkjobs",
    "-g",
    "--group",
    "--files",
}

OPTION_VALUE_PREFIXES = (
    "--file=",
    "--project=",
    "--confirm=",
    "--jobs=",
    "--linkjobs=",
    "--group=",
    "--files=",
)


def split_top_level(command):
    segments = []
    seps = []
    current = []
    in_single = False
    in_double = False
    i = 0
    while i < len(command):
        char = command[i]
        if in_single:
            current.append(char)
            if char == "'":
                in_single = False
            i += 1
            continue
        if in_double:
            if char == "\\" and i + 1 < len(command):
                current.append(command[i : i + 2])
                i += 2
                continue
            current.append(char)
            if char == '"':
                in_double = False
            i += 1
            continue
        if char == "'":
            in_single = True
            current.append(char)
            i += 1
            continue
        if char == '"':
            in_double = True
            current.append(char)
            i += 1
            continue
        if char == "\\" and i + 1 < len(command):
            current.append(command[i : i + 2])
            i += 2
            continue
        if char == "&" and i + 1 < len(command) and command[i + 1] == "&":
            segments.append("".join(current))
            seps.append("&&")
            current = []
            i += 2
            continue
        if char == ";":
            segments.append("".join(current))
            seps.append(";")
            current = []
            i += 1
            continue
        current.append(char)
        i += 1
    segments.append("".join(current))
    return segments, seps


def has_unquoted_shell_meta(segment):
    in_single = False
    in_double = False
    i = 0
    while i < len(segment):
        char = segment[i]
        if in_single:
            if char == "'":
                in_single = False
            i += 1
            continue
        if in_double:
            if char == "\\" and i + 1 < len(segment):
                i += 2
                continue
            if char == '"':
                in_double = False
            i += 1
            continue
        if char == "'":
            in_single = True
            i += 1
            continue
        if char == '"':
            in_double = True
            i += 1
            continue
        if char in "&|<>()":
            return True
        if char == "\\" and i + 1 < len(segment):
            i += 2
            continue
        i += 1
    return False


def option_has_inline_value(token):
    return token.startswith(OPTION_VALUE_PREFIXES) or (
        len(token) > 2 and (token.startswith("-j") or token.startswith("-g"))
    )


def parse_build_args(tokens):
    options = []
    targets = []
    i = 2
    while i < len(tokens):
        token = tokens[i]
        if token in {"-a", "--all"}:
            return None
        if token == "--":
            targets.extend(tokens[i + 1 :])
            break
        if token.startswith("-"):
            options.append(token)
            if token in OPTION_VALUE_FLAGS and not option_has_inline_value(token):
                if i + 1 >= len(tokens):
                    return None
                options.append(tokens[i + 1])
                i += 2
                continue
            i += 1
            continue
        targets.append(token)
        i += 1
    return options, targets


def expand_segment(segment):
    stripped = segment.strip()
    if not stripped or has_unquoted_shell_meta(stripped):
        return segment
    try:
        tokens = shlex.split(stripped, posix=True, comments=False)
    except ValueError:
        return segment
    if len(tokens) < 3 or tokens[0] != "xmake" or tokens[1] not in {"build", "b"}:
        return segment
    parsed = parse_build_args(tokens)
    if parsed is None:
        return segment
    options, targets = parsed
    if len(targets) < 2:
        return segment
    prefix = ["xmake", "build", *options]
    return " && ".join(shlex.join([*prefix, target]) for target in targets)


def expand_command(command):
    if not command or "`" in command or "$(" in command:
        return command, False
    segments, seps = split_top_level(command)
    rewritten = []
    changed = False
    for segment in segments:
        expanded = expand_segment(segment)
        rewritten.append(expanded)
        changed = changed or expanded != segment
    if not changed:
        return command, False
    output = rewritten[0].strip()
    for index, sep in enumerate(seps):
        output = f"{output} {sep} {rewritten[index + 1].strip()}"
    return output, True


def get_path(data, path):
    value = data
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return None
        value = value[key]
    return value if isinstance(value, str) else None


def extract_command(data):
    candidates = [
        ("command", ("tool_input", "command")),
        ("cmd", ("tool_input", "cmd")),
        ("command", ("tool_input", "arguments", "command")),
        ("cmd", ("tool_input", "arguments", "cmd")),
        ("command", ("tool_input", "args", "command")),
        ("cmd", ("tool_input", "args", "cmd")),
        ("command", ("command",)),
        ("cmd", ("cmd",)),
    ]
    for key, path in candidates:
        value = get_path(data, path)
        if value:
            return value, key
    return "", "command"


def hook_payload(original, rewritten, key):
    message = (
        "xmake build only accepts one target per invocation; "
        "multi-target build segments were expanded into chained single-target builds."
    )
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "updatedInput": {key: rewritten},
            "additionalContext": message,
        },
        "systemMessage": f"xmake build auto-expanded: {original} -> {rewritten}",
    }


def run(input_text):
    if not input_text.strip():
        return 0
    try:
        data = json.loads(input_text)
    except json.JSONDecodeError:
        return 0
    command, key = extract_command(data)
    rewritten, changed = expand_command(command)
    if not changed:
        return 0
    print(json.dumps(hook_payload(command, rewritten, key), separators=(",", ":")))
    return 0


def self_test():
    cases = {
        "xmake build test-core test-tool": "xmake build test-core && xmake build test-tool",
        " xmake build -r test-core test-tool ": "xmake build -r test-core && xmake build -r test-tool",
        "xmake build -j 8 test-core test-tool": "xmake build -j 8 test-core && xmake build -j 8 test-tool",
        "xmake build --jobs=8 test-core test-tool": "xmake build --jobs=8 test-core && xmake build --jobs=8 test-tool",
        "xmake b test-core test-tool": "xmake build test-core && xmake build test-tool",
        "xmake build test-core test-tool && xmake run test-core": (
            "xmake build test-core && xmake build test-tool && xmake run test-core"
        ),
        "xmake build test-core test-tool; xmake run test-tool": (
            "xmake build test-core && xmake build test-tool ; xmake run test-tool"
        ),
        "xmake build test-core": "xmake build test-core",
        "xmake build -a test-core test-tool": "xmake build -a test-core test-tool",
        "xmake build test-core test-tool 2>&1 | tail -5": (
            "xmake build test-core test-tool 2>&1 | tail -5"
        ),
        "xmake build -j$(nproc) test-core test-tool": (
            "xmake build -j$(nproc) test-core test-tool"
        ),
    }
    for source, expected in cases.items():
        actual, _ = expand_command(source)
        if actual != expected:
            raise AssertionError(f"{source!r}: expected {expected!r}, got {actual!r}")
    payload_input = json.dumps({"tool_input": {"command": "xmake build a b"}})
    data = json.loads(payload_input)
    command, key = extract_command(data)
    rewritten, changed = expand_command(command)
    assert changed
    payload = hook_payload(command, rewritten, key)
    assert payload["hookSpecificOutput"]["updatedInput"]["command"] == "xmake build a && xmake build b"


if "--self-test" in sys.argv:
    self_test()
    sys.exit(0)

sys.exit(run(os.environ.get("HOOK_INPUT", "")))
PY
