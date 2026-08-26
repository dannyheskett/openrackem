# openrackem

A rack-sorting card game written in C with raylib: draw, exchange, and race to
get your rack of ten cards into ascending order before your opponents. Full
official rules and scoring, AI opponents in three strengths, and the Bonus and
Partners variants.

Targets: Linux, Windows, macOS, Web (WASM), Android, and iOS (native Metal).

*This README is a scaffold placeholder; the full build/play documentation lands
with the polish milestone (M6 in `docs/PLAN.md`).*

## Building

```bash
./scripts/build_raylib_linux.sh   # once: build the pinned raylib
make                              # dev build -> build/openrackem
make run
make test                         # headless unit tests (no window needed)
```

## License

MIT (see `LICENSE`). Third-party components are listed in `NOTICE`.
openrackem is an original implementation of a rack-sorting card game, not
affiliated with or endorsed by any other game or its rights holders.
