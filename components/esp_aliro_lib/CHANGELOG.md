# Changelog

## 1.3.0 (2026-10-14)

### Added

- Support for ESP-IDF 6.1; the supported range is now ESP-IDF 5.3 through 6.1.
- In-session fast-to-standard authentication continuation.
- Access Document request encoding, structural parsing, and verification.
- Application-provided Credential Issuer CA authentication for Access Documents containing `x5chain` certificates.

### Changed

- Expedited-session APIs return the authenticated Access Credential public key and optional `CredentialSignedTimestamp`.
- Fast-transaction persistence provided by the application through storage delegates and explicit commit and clear APIs.
- Key-slot and key-conversion utility APIs renamed with the `esp_aliro_utils_` prefix.

### Fixed

- AUTH1 rejection propagation and failure signaling.
- Stricter response TLV ordering, mandatory field lengths, and timestamp validity.
- Big-endian message attribute encoding and protected-message counter handling after decryption failures.
- Reader certificate compaction and profile bounds.
- APDU response reassembly limit raised to 4096 bytes of response data.
- Revocation Documents accepted alongside the Access Document in DeviceResponse; they are skipped.
- Keys outside the Aliro profile rejected at every DeviceResponse level.
- EXCHANGE mailbox commands tag corrected to 0xBA.

## 1.2.0 (2026-08-11)

- Added NFC APDU command and response chaining with extended-length capability negotiation.
- Added Reader Descriptor configuration and reporting through EXCHANGE and failure CONTROL FLOW commands.

## 1.1.0 (2026-07-30)

- Added support for ESP32-C61.
- Restricted supported ESP-IDF versions to 5.2 through 6.0.
- Added prebuilt libraries for each supported ESP-IDF versions.

## 1.0.1 (2026-06-26)

- Added APIs for retrieving reader group identifiers and public keys.
- Added helpers for converting raw public and private keys to PEM format.

## 1.0.0 (2026-06-01)

- Initial release.
