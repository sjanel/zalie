# Saved model and JSON

- **The saved model is made of aggregates reflected by glaze** (no hand-written list of fields): no constructors, no
  private data members. Enums get their names from a `glz::meta`.
- **glaze stays private to `zalie_core`, and a `glz::meta` never goes in the header of its type** (glaze costs a lot of
  compile time per translation unit): it goes in `src/core/src/model-glaze.hpp`, only included by the translation units
  doing JSON I/O.
- Reading is lenient (`kReadOpts`): unknown keys are skipped and missing keys keep their defaults, so that files written
  by another version load fine. The input is a `RawChars` without terminating null: always read with these options.
- **A new field of a saved struct appears in every object of every file**: collections stay small and git friendly, so
  keep it out of the file while unused.
- A new field also updates the `trivially_relocatable` declaration of its struct, the round trips of
  `tests/core/persistence_test.cpp`, and `docs/reference/file-format.md`.
