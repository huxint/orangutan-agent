# bench-cli

`bench-cli` measures early CLI dispatch overhead before the agent loop exists.

Current A-vs-B comparison:

- `cli.single_shot_prompt`: parses and dispatches `--prompt <text>` in quiet mode.
- `cli.repl_empty`: dispatches the default empty REPL shell in quiet mode.
