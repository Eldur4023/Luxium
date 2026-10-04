# Luxium

A web browser for HTML + CSS + **LuxScript** — no JavaScript.

Luxium is the client-side sibling of [Lux](../Lux): where Lux serves pages
whose logic is written in Lux Script, Luxium renders pages whose logic is also
written in Lux Script. Same language, same grammar, same bytecode VM — the
reserved objects change (`request`, `app`, `routes` → `document`, `page`),
because the context is a browser, not a server.

## Status: M0 — skeleton (planning phase)

Right now Luxium is a **plan** plus a **compilable skeleton** that proves the
two foundational pipelines:

- **HTML → DOM**: tokenizer + tree builder (simplified WHATWG model: raw-text
  mode for `<script>`/`<style>`, implied `html`/`head`/`body`, void elements,
  basic implied end tags, error recovery).
- **LuxScript → tokens**: indentation-aware lexer for the Lux Script grammar
  (placeholders until the real lexer/parser/VM are vendored from Lux in M2).

No rendering, no CSS, no networking, no UI yet. See
[docs/ROADMAP.md](docs/ROADMAP.md).

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/luxium examples/script-demo.html
```

Or just `./compile.sh`.

## Layout

```
docs/               the plan: architecture, roadmap, LuxScript-in-browser spec
src/dom/            DOM tree (Node, Document)
src/html/           HTML tokenizer + tree builder
src/luxscript/      Lux Script frontend skeleton (lexer; parser/VM come from Lux)
src/main.cpp        `luxium` CLI: parse a file, dump the DOM tree, lex scripts
examples/           sample pages
```

## Documents

| Document | What it covers |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, components, engine strategy, sandbox model |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M0–M6 with scope and acceptance criteria |
| [docs/LUXSCRIPT.md](docs/LUXSCRIPT.md) | How Lux Script embeds in a page and what changes vs. the server |
