# RC76xx Requirements — Host Interface (UART)

Primary references:
- `41113440 RC76xx Product Technical Specification r18.pdf`
- Sections: 4.6, Table 4-6, 4.11, 7.5, 4.22/4.29

| ID | Requirement | Source | Rationale | Verification | Related |
|---|---|---|---|---|---|
| REQ-RC76XX-UART-001 | The host **MUST** implement primary UART on `UART1` (8-wire) only; `UART2` **MUST NOT** be used for product data paths (reserved for Semtech internal debug). | PTS r18 §4.6 | Prevents dependency on non-productized debug interface behavior. | Schematic and firmware interface review. |  |
| REQ-RC76XX-UART-002 | If hardware flow control is enabled, host firmware **MUST** honor `UART1_RTS`/`UART1_CTS` handshaking. | PTS r18 §4.6, Table 4-6 | Prevents data overruns and command channel instability at higher baud rates. | Serial stress test across supported baud rates. |  |
| REQ-RC76XX-UART-003 | The design **MUST NOT** add external pull-ups on `UART1_RI`, `UART1_DCD`, or `UART1_DSR`. | PTS r18 Table 4-6 notes | Vendor warns boot failure risk if these lines are externally pulled up. | Schematic/ERC review. |  |
| REQ-RC76XX-UART-004 | The design **SHOULD** provide configurable host-side pulls (typ. ~100 kΩ) on `UART1_TX`/`UART1_RX` to avoid ambiguous levels in host low-power states. | PTS r18 §4.6 note | Reduces undefined signaling and leakage interactions across host/modem sleep states. | Schematic review + low-power idle measurement. | #16 |
| REQ-RC76XX-UART-005 | Host firmware **MUST** use `UART1_DTR` control consistent with power policy, because this line can prevent sleep and can wake the module in Sleep state. | PTS r18 Table 4-6 (`UART1_DTR`), §4.11 | Improper DTR behavior increases power consumption and can defeat sleep targets. | Firmware code inspection + sleep current test matrix. |  |
| REQ-RC76XX-UART-006 | The hardware design **SHOULD** expose UART1 test/debug access (directly or via test points) for integration and failure analysis. | PTS r18 §7.5, §4.22 Table 4-29 | Strongly improves bring-up, manufacturing diagnostics, and field triage. | PCB review + manufacturing test procedure check. |  |
