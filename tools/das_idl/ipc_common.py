"""IPC code generation shared utilities.

Consolidates FNV-1a hashing and built-in interface IDs used by
das_ipc_stub_generator, das_ipc_proxy_generator, and aggregate_ipc.
"""

import struct

# ---------------------------------------------------------------------------
# FNV-1a 32-bit hash of a Windows GUID binary layout
# ---------------------------------------------------------------------------

FNV_PRIME = 0x01000193
FNV_OFFSET_BASIS = 0x811C9DC5


def fnv1a_hash_guid(guid_str: str) -> int:
    """Compute FNV-1a 32-bit hash of a GUID's binary representation.

    Must stay in sync with C++ ``RemoteObjectRegistry::ComputeInterfaceId``
    which hashes the raw ``GUID`` struct bytes (little-endian).

    Accepts both ``{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}`` and
    ``xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`` forms.
    """
    cleaned = guid_str.strip().strip("{}")
    parts = cleaned.split("-")
    if len(parts) != 5:
        raise ValueError(f"Invalid GUID format: {guid_str}")

    data1 = int(parts[0], 16)
    data2 = int(parts[1], 16)
    data3 = int(parts[2], 16)
    data4 = bytes.fromhex(parts[3] + parts[4])

    # little-endian layout matching Windows GUID struct
    binary = (
        struct.pack("<I", data1)
        + struct.pack("<H", data2)
        + struct.pack("<H", data3)
        + data4
    )

    hash_value = FNV_OFFSET_BASIS
    for byte in binary:
        hash_value ^= byte
        hash_value = (hash_value * FNV_PRIME) & 0xFFFFFFFF

    return hash_value


# ---------------------------------------------------------------------------
# Built-in interface IDs (FNV-1a hash of GUID, pre-computed)
#
# These interfaces are defined in DasBasicTypes.idl and are NOT present in the
# regular document.interfaces list at generation time, so their UUIDs cannot
# be looked up dynamically.  The hashes here must match what C++ code uses.
# ---------------------------------------------------------------------------

BUILTIN_INTERFACE_HASHES: dict[str, int] = {
    "IDasBase": 0x7BC07313,
}
