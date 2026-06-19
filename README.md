<div align="center">

# ⚗️ Alkahest

**The universal solvent for text.**

*CyberChef, but native and in your shell.*

[![CI](https://github.com/USERNAME/alkahest/actions/workflows/ci.yml/badge.svg)](https://github.com/USERNAME/alkahest/actions/workflows/ci.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](https://en.cppreference.com/w/cpp/17)

</div>

---

Alkahest is a fast, offline, **recipe-driven text transformation multitool** for the terminal. Pipe text through composable operations, chain them into reusable recipes, explore interactively in a live TUI, or spin up a local web UI — all from a single dependency-free binary.

You know the moment: you've got a blob of *something* — base64? hex? URL-encoded? — and you reach for an online decoder, paste in data you maybe shouldn't be pasting into a website, and click through five steps. Alkahest is that, except it's a 2 MB binary on your machine, it never phones home, and the whole pipeline is one line you can save and reuse.

```console
$ echo -n "VGhlIHF1aWNrIGJyb3duIGZveA==" | alk detect
Detected 1 candidate (most likely first):

1. base64
   The quick brown fox
```

```console
$ echo -n "Hello World" | alk snake-case + to-hex
68656c6c6f5f776f726c64
```

## Why you might actually want this

- **It's offline and private.** Your data never leaves your machine. No "paste your auth token into this website" moments.
- **It composes.** Operations chain with a literal `+`, Unix-pipe style: `alk split , + sort + unique + join ,`.
- **It detects.** Point `alk detect` at a mystery string and it guesses the encoding, ranking candidates by how plausible the decoded output looks.
- **Recipes are just files.** Save a pipeline as a `.alk` file, commit it, share it, run it with `alk run cleanup.alk`. Reproducible text surgery.
- **It has a UI when you want one.** `alk serve` gives you a local web app; `alk tui` gives you an interactive recipe builder in the terminal.
- **It's one static binary.** No runtime, no `node_modules`, no Python venv. C++17, zero external dependencies. Hand-rolled hashing, JSON, and an arithmetic evaluator — nothing to `pip install`.
- **82 operations** across encoding, hashing, text manipulation, data wrangling, and analysis.

## Install

### Build from source

You need a C++17 compiler and CMake ≥ 3.16.

```bash
git clone https://github.com/USERNAME/alkahest.git
cd alkahest
cmake -S . -B build
cmake --build build
sudo cmake --install build      # optional; installs `alk` to /usr/local/bin
```

The binary lands at `build/alk`. Run the test suite with:

```bash
ctest --test-dir build --output-on-failure
```

## Quick tour

Alkahest reads from **stdin** and writes to **stdout**, so it drops straight into your existing pipelines.

```bash
# Encode / decode
echo -n "hello" | alk to-base64                 # aGVsbG8=
echo -n "aGVsbG8=" | alk from-base64            # hello
echo -n "https://a.b/?x=1 2" | alk url-encode   # https%3A%2F%2Fa.b%2F%3Fx%3D1%202

# Hashes (no OpenSSL required)
echo -n "abc" | alk sha256
# ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad

# Hex with chaining
echo -n "Hello World" | alk to-hex + to-upper    # 48454C4C4F20...

# Text wrangling
printf 'banana\napple\ncherry\n' | alk sort
cat names.txt | alk unique + sort + number-lines

# Identifier case conversion
echo -n "user_profile_id" | alk camel-case        # userProfileId
echo -n "GetHTTPResponse" | alk snake-case        # get_http_response

# Data utilities
echo -n "1536000" | alk humanize-bytes            # 1.46 MiB
echo -n "4MiB" | alk to-bytes                     # 4194304
alk calc "sqrt(2)^2 + 1"                          # 3
echo -n '{"b":2,"a":1}' | alk json-pretty
```

### The `+` separator

A standalone `+` token chains operations — the output of one becomes the input of the next:

```bash
echo -n "  MESSY   text  " | alk squeeze-spaces + trim + to-lower
# messy text
```

If an argument contains spaces, quote it for your shell as usual:

```bash
echo -n "foo bar baz" | alk replace "foo bar" qux        # qux baz
printf 'a,b,c\n' | alk split , + sort + join ,           # a,b,c
```

## Recipes

A recipe is a plain-text file: one operation per line, `#` for comments, blank lines ignored. (Inside a recipe file, each *line* is one operation — no `+` needed.)

```alk
# cleanup.alk — normalize a messy list
trim-lines
remove-empty-lines
to-lower
sort
unique
```

Run it:

```bash
cat raw.txt | alk run cleanup.alk
```

See [`examples/`](examples/) for more: `slugify.alk`, `decode-payload.alk`, `tidy-list.alk`, and others.

## `detect` — the magic decoder

Hand `detect` something inscrutable and it tries the usual suspects (hex, decimal bytes, binary, URL-encoding, base64, base64url, Morse), decodes each, and ranks them by how printable the result looks:

```console
$ echo -n "48656c6c6f" | alk detect
Detected 1 candidate (most likely first):

1. hex
   Hello
```

It deliberately *won't* claim ROT13 on ordinary English text — ROT13 of plain text is indistinguishable from plain text, so guessing it would be noise. When nothing decodes cleanly, it says so.

## `serve` — local web UI

```bash
alk serve              # http://127.0.0.1:8744
alk serve --port 9000
```

A single-page app with input, recipe, and output panes that calls a small JSON API (`POST /api/run`). Everything runs locally — the server binds to loopback by default. This is also *why Alkahest is AGPL-licensed* (see below).

## `tui` — interactive builder

```bash
echo -n "some text" | alk tui
```

An interactive recipe builder: add, edit, move, and delete steps while watching the live output update after every change. Write the finished pipeline out to a `.alk` file when you're happy with it.

## Operation catalog

Run `alk list` for the full, always-current catalog with usage strings. The categories:

| Category | What's in it |
|----------|--------------|
| **encoding** | base64 / base64url / base32, hex, binary, char-codes, URL, HTML entities, Morse, NATO phonetic, ROT13, Caesar |
| **hashing** | MD5, SHA-1, SHA-256, SHA-512, CRC-32 |
| **text** | case conversions (upper/lower/title/camel/pascal/snake/kebab/constant), sort / unique / shuffle, head / tail, filter, replace & regex-replace, split / join, pad, indent, trim, strip-ANSI |
| **data** | JSON pretty/minify, arithmetic `eval`, base conversion, byte humanization, Unix-time ⇄ ISO-8601, shell escaping |
| **analysis** | `detect`, `stats`, character frequency, Shannon entropy, line/word/char counts, lorem ipsum |

Most operations have short aliases (`hex` for `to-hex`, `b64` for `to-base64`, `freq` for `char-frequency`, `magic` for `detect`, …). `alk help <op>` shows usage for a single operation.

## Why AGPL-3.0?

Alkahest can run as a network service (`alk serve`). The GNU **Affero** GPL exists precisely for software that's used over a network: it closes the "SaaS loophole" in the ordinary GPL by requiring that users interacting with a modified version over a network be offered its source. If you fork Alkahest and host it for others, they get the same freedoms you did. That's the intent, and that's why the Affero variant rather than plain GPL. See [LICENSE](LICENSE).

## Contributing

Adding an operation is deliberately easy — each one is a small lambda registered with a fluent builder. See [CONTRIBUTING.md](CONTRIBUTING.md) for the walkthrough.

## License

Copyright © 2026 the Alkahest authors. Licensed under the GNU Affero General Public License v3.0 or later. See [LICENSE](LICENSE).
