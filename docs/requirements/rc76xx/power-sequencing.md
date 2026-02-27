# RC76xx Requirements — Power Sequencing

Primary references:
- `41113440 RC76xx Product Technical Specification r18.pdf`
- Sections: 3.2.1, 3.2.2, 4.1, 4.1.1, 4.1.2, 4.2, 4.3, 4.14, 4.24.5

| ID | Requirement | Source | Rationale | Verification | Related |
|---|---|---|---|---|---|
| REQ-RC76XX-PWR-001 | The host **MUST** assert `POWER_ON_N` low for **>=200 ms and <~7 s** to power on RC76xx after VBATT is valid. | PTS r18 §4.1, Table 4-2 | Outside this window, startup can fail or produce unintended power-cycle behavior. | Bring-up test with scope capture of `VBATT`, `POWER_ON_N`, boot success criteria. |  |
| REQ-RC76XX-PWR-002 | The host **MUST** use open-drain/open-collector behavior on `POWER_ON_N` and **MUST NOT** add an external pull-up on that pin. | PTS r18 §4.1 | The pin has internal pull-up behavior; external pull-up can violate intended control levels. | Schematic review + PCB netlist/ERC check. |  |
| REQ-RC76XX-PWR-003 | Until `VGPIO` is present (and whenever module is off/reset/updating), host-connected I/Os **MUST** be High-Z, floating, or input pull-down. | PTS r18 §4.1.1.1 note + §3.2.4 note | Prevents back-powering, undefined logic injection, and extra current draw. | Firmware GPIO-state test + hardware design review. | #16 |
| REQ-RC76XX-PWR-004 | During shutdown, host firmware **MUST** wait for safe-off indication (`VGPIO` low and `SAFE_PWR_REMOVE` low, or validated equivalent delay) before removing VBATT. | PTS r18 §4.1.2 | Early power cut increases flash/EFS corruption risk. | Controlled shutdown test with repeated cycles and filesystem integrity checks. | #15, #16 |
| REQ-RC76XX-PWR-005 | The design **SHOULD** expose (directly or indirectly) `VGPIO` and/or `SAFE_PWR_REMOVE` to host firmware so safe power removal can be implemented deterministically. | PTS r18 §4.1.2, §4.14, §4.24.5 | Observability simplifies robust shutdown policy versus fixed delays alone. | Schematic/firmware interface review. | #15 |
| REQ-RC76XX-PWR-006 | The system power architecture **MUST** tolerate UVLO behavior: if VBAT falls below threshold, module powers down and remains off until VBAT is valid and ON/OFF control is active again. | PTS r18 §3.2.1, Table 3-4 | Brownouts are expected in field power systems; host state machine must recover cleanly. | Brownout injection test + recovery state-machine test. |  |
| REQ-RC76XX-PWR-007 | The host **SHOULD** avoid abrupt VBAT removal during operation and **SHOULD** prefer graceful shutdown flow to minimize EFS crash risk. | PTS r18 §3.2.2, §4.1.2, §4.3 note b | Abrupt loss can corrupt persistent storage and create boot/reliability issues. | Long-run power-cycle reliability test (graceful vs abrupt). |  |
| REQ-RC76XX-PWR-008 | `RESET_IN_N`-based emergency power-off (`POWER_ON_N` de-asserted + `RESET_IN_N` low >=8 s) **MUST** be reserved for fault recovery only. | PTS r18 §4.2, §4.3, §4.15 note | Hard reset/off path can risk memory corruption if used as normal control path. | Fault-injection scenario test + firmware policy/code inspection. |  |

## Open question (spec consistency)

The spec includes both:
- §4.1.1.1 note c: power-off must be initiated using `POWER_ON_N`; AT-command initiation "not supported".
- §4.1.2 and §4.3 note b: `AT!POWERDOWN` is described and strongly recommended for graceful shutdown.

Current interpretation in these requirements: treat **graceful AT-initiated shutdown + safe power removal gating** as preferred behavior, while maintaining `POWER_ON_N` hardware control path for deterministic fallback.
