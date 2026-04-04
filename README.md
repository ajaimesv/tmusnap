# tmusnap

A tool to save and restore tmux sessions.

## Building it

```shell
c++ -std=c++17 -O2 -Wall -Wextra -pedantic tmusnap.cpp -o tmusnap
```

## Running it

> **Important:** Using `--force` will remove any existing session with the same name.
> Think twice before using it.

```shell
Usage:
  tmusnap --save [--verbose]
    Write snapshot JSON to stdout.

  tmusnap --restore [--force] [--verbose]
    Read snapshot JSON from stdin and restore it.
    --force: overwrite any existing sessions with the same names. Be careful.

Examples:
  tmusnap --save > tmux_snapshot.json
  tmusnap --restore < tmux_snapshot.json
```

I recommend you to restore a session first, and then attach to it, like in:
```shell
tmusnap --restore < tmux_snapshot.json
tmux attach
```

## Third party notices

This project uses [nlohmann/json](https://github.com/nlohmann/json), a JSON library for modern C++, licensed under the MIT License.

