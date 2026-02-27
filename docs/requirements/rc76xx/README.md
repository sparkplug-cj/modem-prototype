# RC76xx Host Integration Requirements (SRS-lite)

This directory captures **host-side integration requirements** for RC76xx modules, distilled from the product technical specification.

Source baseline:
- `docs/datasheets/modems/swir/rc7620-1/41113440 RC76xx Product Technical Specification r18.pdf`

## Categories

- [Power sequencing](./power-sequencing.md)
- [Host interface (UART)](./host-interface-uart.md)
- [Sleep and PSM](./sleep-psm.md)
- [Firmware update and recovery](./firmware-update.md)

## Requirement format

Each requirement includes:
- Unique ID (`REQ-RC76XX-<CAT>-NNN`)
- Normative statement (MUST/SHOULD)
- Source reference (document + section)
- Rationale
- Verification method
- Related issue(s), where applicable

## Notes

- Requirements are paraphrased to avoid reproducing long copyrighted excerpts.
- Where the vendor text appears internally inconsistent, we record conservative requirements and call out open questions.
