# Research notes (2026-10-31)

The deep-research notes behind [../RESEARCH-BLE.md](../RESEARCH-BLE.md), the report that chose the design in
[../ARCHITECTURE.md](../ARCHITECTURE.md). Written before the project existed, under an earlier assumption that a
Wi-Fi station stays connected; the report says where the no-Wi-Fi decision changes their conclusions. Citations
point into ESP-IDF v5.5.4 at `C:\Espressif\esp-idf` with the line numbers as read then.

| File | Question |
|---|---|
| [advertising-flood-nimble.md](advertising-flood-nimble.md) | A custom connectionless advertising flood with NimBLE: parameters, payload, loss model, reliability |
| [sig-mesh-esp-ble-mesh.md](sig-mesh-esp-ble-mesh.md) | Bluetooth SIG mesh through ESP-BLE-MESH: provisioning without a phone, latency, payload, known issues |
| [gatt-connections-and-esp-now.md](gatt-connections-and-esp-now.md) | GATT connection topologies on the S3, and ESP-NOW as the non-Bluetooth reference |
| [measured-latency-and-demos.md](measured-latency-and-demos.md) | Measured latency and reliability of small BLE shared-state systems; what "instant" means to a person |
| [ram-flash-and-coexistence.md](ram-flash-and-coexistence.md) | RAM, flash, PSRAM eligibility and Kconfig for the three designs; Wi-Fi coexistence |
| [ble-peer-firmware-update.md](ble-peer-firmware-update.md) | A board pulling a newer firmware from a peer over BLE: GATT transfer, OTA writes, rollback |
