# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import asyncio
import hashlib
import os
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import cbor2

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src"))

from aliro_actuator.access_protocol.defines import TransportProtocol
from aliro_actuator.access_protocol.user_device import UserDevice, UserMode
from aliro_actuator.trust_framework.access_credential import AccessCredential
from aliro_actuator.trust_framework.key import KeyPair, PrivateKey, PublicKey
from examples.nfc.common import READER_GROUP_IDENTIFIER


def create_access_document(
    credential_public_key: PublicKey, issuer_private_key: PrivateKey
) -> bytes:
    credential_key = credential_public_key.as_bytes()
    issuer_key = issuer_private_key.generate_public_key().as_bytes()
    if any(len(key) != 65 or key[0] != 0x04 for key in (credential_key, issuer_key)):
        raise ValueError("the demo requires uncompressed P-256 public keys")

    item = cbor2.CBORTag(
        24,
        cbor2.dumps(
            {
                "1": 0,  # digestID
                "2": os.urandom(16),  # random
                "3": "floor1",  # elementIdentifier
                "4": {0: 1, 4: [1]},  # AccessData Version 1, provisioned ReaderRuleIds [1]
            },
            canonical=True,
        ),
    )
    # Sample once so signed == validFrom; restarting also refreshes the document timestamp.
    issued_at = datetime.now(timezone.utc).replace(microsecond=0)
    signed_time = issued_at.strftime("%Y-%m-%dT%H:%M:%SZ")
    valid_until = (issued_at + timedelta(days=365)).strftime("%Y-%m-%dT%H:%M:%SZ")
    mso = {
        "1": "1.0",  # version
        "2": "SHA-256",  # digestAlgorithm
        "3": {"aliro-a": {0: hashlib.sha256(cbor2.dumps(item, canonical=True)).digest()}},
        "4": {  # deviceKeyInfo.deviceKey: COSE EC2, P-256, x, y
            "1": {1: 2, -1: 1, -2: credential_key[1:33], -3: credential_key[33:]}
        },
        "5": "aliro-a",  # docType
        "6": {  # validityInfo; Aliro text keys from Table 7-1
            "1": cbor2.CBORTag(0, signed_time),  # signed
            "2": cbor2.CBORTag(0, signed_time),  # validFrom
            "3": cbor2.CBORTag(0, valid_until),  # validUntil
            "5": 1,  # validityIteration
        },
        "7": False,  # timeVerificationRequired
    }
    protected = cbor2.dumps({1: -7}, canonical=True)  # ES256
    payload = cbor2.dumps(
        cbor2.CBORTag(24, cbor2.dumps(mso, canonical=True)), canonical=True
    )
    sig_structure = cbor2.dumps(
        ["Signature1", protected, b"", payload], canonical=True
    )
    issuer_auth = [  # COSE_Sign1: protected, unprotected, payload, signature
        protected,
        {4: hashlib.sha256(b"key-identifier" + issuer_key).digest()[:8]},
        payload,
        issuer_private_key.sign(sig_structure),
    ]
    # Supply one Document; UserDevice wraps it in the DeviceResponse.
    return cbor2.dumps(
        {"1": {"1": {"aliro-a": [item]}, "2": issuer_auth}, "5": "aliro-a"},
        canonical=True,
    )


async def main(*, extended_apdu: bool) -> None:
    keys = Path(__file__).resolve().parent
    reader_public_key = PublicKey(
        (keys / "reader_public_key.pem").read_text(encoding="ascii")
    )
    issuer_public_key = PublicKey(
        (keys / "issuer_public_key.pem").read_text(encoding="ascii")
    )
    issuer_private_key = PrivateKey(
        (keys / "issuer_private_key.pem").read_text(encoding="ascii")
    )
    reader_identifier_list = [
        (READER_GROUP_IDENTIFIER, reader_public_key),
        (bytes.fromhex("00113344667799AA00113344667799AB"), issuer_public_key),
    ]
    credential_keypair = KeyPair(
        (keys / "credential_private_key.pem").read_text(encoding="ascii"),
        (keys / "credential_public_key.pem").read_text(encoding="ascii"),
    )
    device = UserDevice(
        transport_protocol=TransportProtocol.NFC,
        access_credentials=[
            AccessCredential(credential_keypair, reader_identifier_list)
        ],
        mailbox=512,
        access_document=create_access_document(
            credential_keypair.get_public_key(), issuer_private_key
        ),
        revocation_document=None,
        mode=UserMode.USER,
    )
    if extended_apdu:
        device.apdu.set_extended_length(command_length=512, response_length=512)
    await device.main_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Aliro NFC user device for the ESP-IDF reader example."
    )
    parser.add_argument(
        "--extended-apdu",
        action="store_true",
        help="advertise 512-byte extended APDU support (default: short APDU chaining)",
    )
    args = parser.parse_args()
    asyncio.run(main(extended_apdu=args.extended_apdu))
