"""Read the U6 boot partition from an already-running FES session. No flash writes."""
import argparse
import json
from pathlib import Path
import struct
from tempfile import TemporaryDirectory

from check_boot_backup import boot_location, verify_partition

VID, PID = 0x1f3a, 0xefe8
TRANSFER_SIZE = 64 * 1024
TIMEOUT = 8000


class FesReader:
    def __init__(self, send, receive, wire="direct"):
        self.send, self.receive = send, receive
        self.wire = wire
        self.identified = False

    def response(self):
        magic, _, residue, status = struct.unpack("<4sIIB", self.receive(13))
        if magic != b"AWUS" or residue or status:
            raise RuntimeError("Allwinner USB transfer failed")

    def transport(self, direction, length):
        self.send(struct.pack("<8sIIHI10x", b"AWUC", length, 0x0c000000,
                              direction, length))

    def read(self, length):
        self.transport(0x11, length)
        data = self.receive(length)
        self.response()
        return data

    def command(self, request, length):
        self.transport(0x12, len(request))
        self.send(request)
        self.response()
        result = self.read(length)
        mark, _, status = struct.unpack("<HHB3x", self.read(8))
        if mark != 0xffff or status:
            raise RuntimeError("FES rejected the read request")
        return result

    def identify(self):
        data = self.command(struct.pack("<HH12x", 1, 0), 32)
        tag, hardware, firmware, mode = struct.unpack_from("<8sIIH", data)
        if tag != b"AWUSBFEX" or hardware not in (0x168900, 0x161000) or mode != 2:
            raise RuntimeError("USB device is not in the expected Allwinner FES mode")
        self.identified = True
        return {"hardware_id": hex(hardware), "firmware_id": hex(firmware), "fes_mode": mode}

    def reboot(self):
        if not self.identified:
            raise RuntimeError("FES identity must be verified before requesting reboot")
        # TOOL_MODE_REBOOT for both the tool mode and next action.
        self.send(struct.pack("<HHIII4s", 0x020f, 0, 2, 2, 0, b"AWUC"))
        self.response()

    def sectors(self, lba, byte_count):
        if byte_count % 512:
            raise ValueError("FES reads require sector alignment")
        data = bytearray()
        for offset in range(0, byte_count, TRANSFER_SIZE):
            count = min(TRANSFER_SIZE, byte_count - offset)
            # 0x0207 is FES upload (device to host), with the flash rather than DRAM tag.
            flags = 0x8000 | (0x10000 if offset + count == byte_count else 0)
            request = struct.pack("<HHIII", 0x0207, 0, lba + offset // 512, count, flags)
            if self.wire == "direct":
                self.send(request + b"AWUC")
                data.extend(self.receive(count))
                self.response()
            else:
                data.extend(self.command(request, count))
        return bytes(data)


def backup(reader, directory):
    result = reader.identify()
    if directory is None:
        return result
    if directory.exists():
        raise FileExistsError(directory)
    table = reader.sectors(0, 4 * 16384)
    layout = boot_location(table)
    first = reader.sectors(layout["boot_logical_lba"], layout["partition_bytes"])
    result.update(layout, **verify_partition(first))
    if first != reader.sectors(layout["boot_logical_lba"], layout["partition_bytes"]):
        raise RuntimeError("two boot readbacks differ")
    result["identical_boot_readbacks"] = 2
    with TemporaryDirectory(prefix="u6-boot-readback-", dir=directory.parent) as temporary:
        staged = Path(temporary) / "verified"
        staged.mkdir()
        (staged / "sunxi-mbr.bin").write_bytes(table)
        (staged / "boot-partition.bin").write_bytes(first)
        (staged / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
        if directory.exists():
            raise FileExistsError(directory)
        staged.rename(directory)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libusb", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="new directory for the two-pass boot readback")
    parser.add_argument("--wire", choices=("direct", "legacy"), default="direct",
                        help="FES transfer framing; direct matches the public libefex source")
    parser.add_argument("--reboot", action="store_true", help="reboot after an identified FES session")
    args = parser.parse_args()

    import usb.backend.libusb1
    import usb.core
    import usb.util

    backend = usb.backend.libusb1.get_backend(find_library=lambda _: str(args.libusb.resolve()))
    if backend is None:
        parser.exit(2, "Could not load libusb\n")
    devices = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID, backend=backend))
    if len(devices) != 1:
        parser.exit(2, f"Expected one device at {VID:04x}:{PID:04x}; found {len(devices)}\n")

    device = devices[0]
    candidates = []
    for interface in device.get_active_configuration():
        if interface.bAlternateSetting != 0:
            continue
        bulk = [ep for ep in interface if usb.util.endpoint_type(ep.bmAttributes) == 2]
        directions = {usb.util.endpoint_direction(ep.bEndpointAddress): ep for ep in bulk}
        if len(bulk) == len(directions) == 2:
            candidates.append((interface, directions[0], directions[128]))
    if len(candidates) != 1:
        parser.exit(2, "Ambiguous Allwinner USB bulk interface\n")
    interface, output, incoming = candidates[0]

    def send(data):
        if output.write(data, timeout=TIMEOUT) != len(data):
            raise RuntimeError("short USB command")

    def receive(length):
        data = bytes(incoming.read(length, timeout=TIMEOUT))
        if len(data) != length:
            raise RuntimeError("short USB read")
        return data

    usb.util.claim_interface(device, interface.bInterfaceNumber)
    try:
        reader = FesReader(send, receive, args.wire)
        try:
            print(json.dumps(backup(reader, args.output), indent=2), flush=True)
        finally:
            if args.reboot and reader.identified:
                reader.reboot()
    finally:
        usb.util.dispose_resources(device)


if __name__ == "__main__":
    main()
