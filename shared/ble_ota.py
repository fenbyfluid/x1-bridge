#!/usr/bin/env python3
import asyncio
import ecdsa
import hashlib
import io
import struct
from argparse import ArgumentParser
from bleak import BleakScanner, BleakClient
from ecdsa.util import sigencode_der

SERVICE_UUID = "00001000-7858-48fb-b797-8613e960da6a"
CHAR_UUID = "00002009-7858-48fb-b797-8613e960da6a"

event = asyncio.Event()
aborted = False


def notification_handler(sender, data):
    global event
    global aborted
    if data == b"\x00":
        print("device aborted update")
        aborted = True
    else:
        print("upgrade complete")
    event.set()


async def main():
    global event
    global aborted

    parser = ArgumentParser(description="sign and upload an ota image over ble")
    parser.add_argument(
        "-k",
        "--sig-key",
        metavar="key",
        help="hex-encoded ecdsa p-256 private key",
        required=True,
    )
    parser.add_argument(
        "firmware",
        help="path to the firmware.bin to upload",
    )
    options = parser.parse_args()

    private_key = bytearray.fromhex(options.sig_key)
    signing_key = ecdsa.SigningKey.from_string(
        private_key, ecdsa.NIST256p, hashlib.sha256
    )

    with open(options.firmware, mode="rb") as file:
        scanner = BleakScanner(service_uuids=[SERVICE_UUID])
        device = await scanner.find_device_by_filter(
            lambda d, ad: d.name and SERVICE_UUID in ad.service_uuids, 60.0
        )
        print(device)

        if device is None:
            return

        async with BleakClient(device, timeout=30.0) as client:
            print("connected")
            await client.start_notify(CHAR_UUID, notification_handler)

            print("authorized, starting update")

            file.seek(0, io.SEEK_END)
            size = file.tell()
            file.seek(0, io.SEEK_SET)

            await client.write_gatt_char(
                CHAR_UUID, struct.pack("<BBI", 1, 1, size), True
            )

            sha256 = hashlib.sha256()

            done = 0
            # 3 bytes GATT overhead, 1 byte for our packet type header
            chunk_size = client.mtu_size - 3 - 1
            for chunk in iter(lambda: file.read(chunk_size), b""):
                if aborted:
                    return

                await client.write_gatt_char(
                    CHAR_UUID, struct.pack("<B", 2) + chunk, True
                )
                done += len(chunk)
                print("{0} / {1} ({2:.2f}%)".format(done, size, (done / size) * 100))

                sha256.update(chunk)

            hash = sha256.digest()
            print("hash: {0}".format(hash.hex()))

            signature = signing_key.sign_digest_deterministic(
                hash, hashlib.sha256, sigencode_der
            )
            print("signature: {0}".format(signature.hex()))

            await client.write_gatt_char(
                CHAR_UUID, struct.pack("<B", 3) + signature, True
            )

            await event.wait()


asyncio.run(main())
