# RC76xx Requirements — Sleep and PSM

Primary references:
- `41113440 RC76xx Product Technical Specification r18.pdf`
- Sections: 3.2.3, 3.2.4, 3.2.5, 4.11, 4.12, 4.14

| ID | Requirement | Source | Rationale | Verification | Related |
|---|---|---|---|---|---|
| REQ-RC76XX-PSM-001 | Host power management **MUST** distinguish Sleep vs PSM wake sources; wake logic for one state **MUST NOT** be assumed valid for the other. | PTS r18 §4.11, §4.12 | Sleep supports additional wake pins; PSM wake is restricted. Misclassification causes missed wake events. | Firmware state-machine review + wake-source integration tests. |  |
| REQ-RC76XX-PSM-002 | When PSM is enabled, `POWER_ON_N` **MUST** be left floating (not hard-grounded), otherwise unintended wake behavior occurs. | PTS r18 §3.2.4 note | Grounding `POWER_ON_N` in PSM defeats low-power dormant behavior. | Board-level validation of `POWER_ON_N` bias + current measurements in PSM. | #16 |
| REQ-RC76XX-PSM-003 | During PSM/off states (where `VGPIO` is off/high-Z), host-connected modem I/Os **MUST** be placed in High-Z, floating, or input pull-down. | PTS r18 §3.2.4 note, §4.14 note, §4.1.1.1 | Avoids leakage/back-powering and preserves ultra-low-power targets. | Firmware low-power GPIO conformance tests + current profiling. | #15, #16 |
| REQ-RC76XX-PSM-004 | The product **MUST** configure PSM through supported control interfaces (`AT+CPSMS` or `AT!POWERMODE`/`AT!POWERWAKE`) and handle network-negotiated timer outcomes rather than assuming requested values are applied as-is. | PTS r18 §3.2.4, §3.2.4/3.2.5 process, Table 3-7 | Network acceptance and timer negotiation directly determine duty-cycle and reachability. | AT/API integration tests with readback and observed cycle timing. |  |
| REQ-RC76XX-PSM-005 | Firmware **SHOULD** expose configurable PSM timers (T3412/T3324) and document latency implications, including unreachable periods during dormancy. | PTS r18 §3.2.4, §3.2.5 important notes | Application behavior and user expectations depend on explicit latency/power trade-offs. | Product configuration review + system test scenarios. |  |
| REQ-RC76XX-PSM-006 | Firmware **MUST NOT** rely on ADC/GPIO wake in PSM state. | PTS r18 §3.2.5 note | Vendor explicitly excludes ADC/GPIO wake in PSM; relying on it breaks wake guarantees. | Code inspection + negative test in PSM. |  |
| REQ-RC76XX-PSM-007 | If PSM entry fails, firmware **MUST** explicitly re-issue PSM request (no implicit auto-retry assumption). | PTS r18 §3.2.5 | Prevents silent high-power operation when one PSM attempt is rejected/missed. | Failure-injection test (forced non-entry) + retry behavior verification. |  |
