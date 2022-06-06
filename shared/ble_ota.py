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
remote_bytes_written = 0

def notification_handler(sender, data):
    global event
    global aborted
    global remote_bytes_written

    progress, succeeded = struct.unpack("<IB", data)

    if progress != 0xFFFFFFFF:
        #print("remote progress: {0}".format(progress))
        remote_bytes_written = progress
        return

    if succeeded:
        print("upgrade complete")
    else:
        print("device aborted update")
        aborted = True

    event.set()

async def main():
    global event
    global aborted
    global remote_bytes_written

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

            # 1029840 bytes
            # True = 8m13.095s
            # False = 3m8.115s
            # 1029920 bytes
            # 0 (confirm every write) = 8m43.180s
            # 1 = 5m51.964s
            # 2 = 4m57.359s
            # 3 = 4m27.429s
            # 4 = 4m11.689s
            # 5 = 4m14.373s
            # 6 = 3m59.222s
            # 7 = 3m48.791s
            # 10 = 3m40.865s
            # 11 = 3m28.005s
            # 15 = 3m26.664s
            # float('inf') (confirm no writes) = 3m30.335s
            # 11 (12 writes to a batch) is a multiple of both 4 (iOS) and 6 (Android) packets per interval
            max_unconfirmed_writes = 11

            await client.write_gatt_char(CHAR_UUID, struct.pack("<BBI", 1, 1, size))

            sha256 = hashlib.sha256()

            done = 0
            unconfirmed_writes = 0
            # 3 bytes GATT overhead, 1 byte for our packet type header
            chunk_size = client.mtu_size - 3 - 1
            for chunk in iter(lambda: file.read(chunk_size), b""):
                if aborted:
                    return

                confirm_this_write = unconfirmed_writes >= max_unconfirmed_writes

                await client.write_gatt_char(
                    CHAR_UUID, struct.pack("<B", 2) + chunk, confirm_this_write
                )
                done += len(chunk)
                print("{0} / {1} ({2:.2f}%) ({3})".format(done, size, (done / size) * 100, "confirmed" if confirm_this_write else "unconfirmed"))

                if not confirm_this_write:
                    unconfirmed_writes += 1
                else:
                    unconfirmed_writes = 0

                sha256.update(chunk)

            hash = sha256.digest()
            print("hash: {0}".format(hash.hex()))

            signature = signing_key.sign_digest_deterministic(
                hash, hashlib.sha256, sigencode_der
            )
            print("signature: {0}".format(signature.hex()))

            await client.write_gatt_char(CHAR_UUID, struct.pack("<B", 3) + signature)

            await event.wait()


asyncio.run(main())
