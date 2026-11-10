# Fidelity: <entry name>

Real design being mimicked: <name and version>

Reference material used (source tree, docs, papers). Pin every source repository to the full 40-character commit hash that was read, also when you name a tag or a branch, because tags and branches move:

- Source: <repo link> at `<full commit hash>` (<tag or branch, shallow or sparse clone>). Files read: <list>.
- Not read: <list>.

## Stage mapping

One row per stage of this entry, in pipeline order. Write each stage as its `stage_*` function name from `main.c` in backticks, for example `stage_lower`. `scripts/check-fidelity.sh` fails if a `stage_*` function is missing from this table, or if the table names one that `main.c` does not have.

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_example` | | | | |

## Deliberate deviations

Everything that differs from the real design on purpose, with the reason.

- <deviation>: <why>

## Comparison with the real tool

If the real tool can print its own intermediate forms, record the command, the equivalent input, and what matches and what does not. Output need not be identical. The stages, their order and the shape of the data should match.

- Command: `<command>`
- Matches: <stages and data shapes that agree>
- Differs: <and why>

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
