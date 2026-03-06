# RC76xx Requirements — Firmware Update and Recovery

Primary references:
- `41113440 RC76xx Product Technical Specification r18`
- Sections: 4.20, 6.2, 6.3, 6.4, 7.7, 4.22/4.29

| ID | Requirement | Source | Rationale | Verification | Related |
|---|---|---|---|---|---|
| REQ-RC76XX-FW-001 | The product **MUST** provide a field firmware upgrade path over USB and/or FOTA (AirVantage/AVMS-capable flow). | PTS r18 §6.3, §6.4 | Firmware and OS updates are expected for fixes/security and standards evolution. | Product update procedure test (local and/or OTA). |  |
| REQ-RC76XX-FW-002 | System design **SHOULD** allow practical USB access for firmware download and debug trace capture (connector, mux path, or test points). | PTS r18 §6.4 tip, §7.7, §4.6 USB note | Without usable USB path, recovery and diagnostics become operationally fragile. | Hardware design review + manufacturing/serviceability test. |  |
| REQ-RC76XX-FW-003 | The design **MUST** expose TP1 boot control at least via a mandatory test point; optional active control is recommended for recovery workflows. | PTS r18 §4.20, Table 4-26, Table 4-29 | TP1 is required for bootloader-mode recovery in non-booting scenarios. | PCB review + controlled bootloader entry test. |  |
| REQ-RC76XX-FW-004 | If TP1 is used for forced bootloader entry, host/service procedures **MUST** de-assert TP1 after bootloader entry so normal boot resumes after download. | PTS r18 §4.20 | Holding TP1 active can prevent return to normal runtime boot. | Service procedure verification + recovery regression test. |  |
| REQ-RC76XX-FW-005 | Firmware update/power-control workflow **MUST** enforce host I/O safe states (High-Z/floating/input pull-down) during reset/update/power-off transitions. | PTS r18 §4.1.1.1, §3.2.4 note | Protects against undefined IO levels and unintended current draw during sensitive update windows. | Firmware procedure review + power/update cycle stress test. | #16 |
| REQ-RC76XX-FW-006 | The platform **SHOULD** include a recovery SOP for repeated-reset conditions (boot-and-hold behavior after consecutive resets) and corresponding firmware reload action. | PTS r18 §6.2 | Field devices can enter protective boot-and-hold behavior after repeated resets; support process is needed. | Operational runbook review + lab simulation of repeated reset trigger. |  |
