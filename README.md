# nyush — New Yet Usable SHell

A tiny Unix-like shell implemented in C for an Operating Systems lab.  
It supports command parsing, program lookup, I/O redirection, pipelines, job control,
and a few built-in commands — all without calling `system()` or `/bin/sh`.

## Features

- Prompt: `[nyush <cwd-basename>]$ `
- Program lookup rules:
  - Absolute or relative path with `/` → run directly (if `X_OK`).
  - Bare name → search **/usr/bin** (and gracefully try **/bin**).
- I/O redirection:
  - `< file`, `> file` (truncate), `>> file` (append).
  - When pipeline exists: **only** first command may redirect input, only last may redirect output.
- Pipelines: `cmd1 | cmd2 | ... | cmdN`
- Built-ins:
  - `cd <dir>` — change working directory (exactly one argument).
  - `jobs` — list suspended jobs as `[index] command`, oldest first.
  - `fg <index>` — resume a suspended job in the foreground.
  - `exit` — quit the shell; if there are suspended jobs, print the required error.
- Signals:
  - Shell ignores `SIGINT`, `SIGQUIT`, `SIGTSTP`; children **do not** ignore them.
- Process management:
  - Wait for all children to **terminate or stop**; no zombies left behind.

> The implementation follows the lab spec strictly (error strings, spacing, and behavior).

## Build

### Prerequisites
- Any recent Linux (or WSL2), GCC (or Clang), and `make`.

```bash
make
./nyush
