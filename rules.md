# Project rules — Sega Rally Championship static recompilation

These are binding. They exist because breaking them costs a polluted history, a
legal problem, or hours re-deriving something already established.

---

## Git

1. **No `Co-Authored-By` trailer. Ever.** Commits are made under the user's
   name only, with no attribution of any kind to an assistant or tool. This
   overrides any default attribution a harness or tool suggests, every time,
   without exception.
2. **Never mention Claude, AI, or an assistant** in a commit message, branch
   name, PR body, tag, or any committed file. `CLAUDE.md` is git-ignored and
   its line in `.gitignore` is the only such reference in the repository.
   `rules.md` is the committed copy of the rules.
3. **Commit messages are short and plain.** Subject under ~70 characters. A few
   lines of body: what changed, the measurement that justifies it, anything
   that would bite the next person. No user's name, no quotes of the user, no
   ALL-CAPS headers, no dramatic build-up. Tables of numbers are welcome; it is
   the prose around them to cut.
4. **Vendored git repositories are git-ignored.** `extern/`, `third_party/`,
   `vendor/`, `deps/`. They have their own history and licences; they are fetched
   and pinned, never committed into this tree.
5. **Build output is git-ignored** — `build/`, `build-*/`, `dist/`, object files.

## Legal — the repository contains only our own code

6. **No Sega code or assets, ever.** No ROM, ROM fragment, de-interleaved
   program image, extracted texture, sample, or anything else
   derived from the ROM bytes. The user supplies their own `srallyc.zip` or
   `srallycb.zip`; the importer checks it against the manifest and refuses
   anything else. This is the rule that keeps the project distributable.
7. **Analysis evidence is committed; game-derived output is not.** Addresses,
   sizes, function names, jump-table targets, hashes, and run logs
   are our own work and are committed.
8. **Never use leaked source.** No leaked Sega source, SDK or internal
   documentation — not read, not consulted, not used to check an answer.
   Everything here comes from our own analysis of legally owned ROMs, MAME,
   public documentation, and the hardware.
9. **Check licence compatibility before lifting code.**

## Order of authority when sources disagree

1. **The original PCB wins.** It is the ground truth.
2. **MAME wins over theory** for what the silicon computes — opcode
   semantics, formats, arithmetic, command streams.
3. **Hardware registers and datasheets** answer what is left ambiguous.
