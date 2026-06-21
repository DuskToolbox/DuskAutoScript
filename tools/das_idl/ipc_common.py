"""IPC code generation shared utilities.

Consolidates FNV-1a hashing, built-in interface IDs, and property-to-method
conversion used by das_ipc_stub_generator, das_ipc_proxy_generator, and
aggregate_ipc.
"""

import importlib
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Parser type imports — support both package import and direct script execution
# ---------------------------------------------------------------------------

try:
    from . import das_idl_parser as _das_idl_parser
except ImportError:
    this_dir = str(Path(__file__).resolve().parent)
    if this_dir not in sys.path:
        sys.path.insert(0, this_dir)
    _das_idl_parser = importlib.import_module("das_idl_parser")

TypeKind = _das_idl_parser.TypeKind
ParamDirection = _das_idl_parser.ParamDirection
MethodDef = _das_idl_parser.MethodDef
ParameterDef = _das_idl_parser.ParameterDef
TypeInfo = _das_idl_parser.TypeInfo

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


def fnv1a_hash(data: str) -> int:
    """Compute FNV-1a 32-bit hash of a UTF-8 encoded string."""
    hash_value = FNV_OFFSET_BASIS
    for byte in data.encode("utf-8"):
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


# ---------------------------------------------------------------------------
# Property → getter/setter MethodDef conversion
# ---------------------------------------------------------------------------

def properties_to_methods(properties: list) -> list:
    """Convert PropertyDef list to MethodDef list (getter/setter pairs)."""
    methods = []
    for prop in properties:
        is_interface = (prop.type_info.type_kind == TypeKind.INTERFACE
                        and prop.type_info.pointer_level == 0)
        is_string = prop.type_info.base_type in ('string', 'IDasReadOnlyString')

        if prop.has_getter:
            if is_interface or is_string:
                out_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=2, is_pointer=True,
                                    type_kind=prop.type_info.type_kind)
                params = [ParameterDef(name="pp_out", type_info=out_type, direction=ParamDirection.OUT)]
            else:
                out_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=1, is_pointer=True,
                                    type_kind=prop.type_info.type_kind)
                params = [ParameterDef(name="p_out", type_info=out_type, direction=ParamDirection.OUT)]
            methods.append(MethodDef(name=f"Get{prop.name}", return_type=TypeInfo(base_type="DasResult", pointer_level=0), parameters=params))

        if prop.has_setter:
            if is_interface:
                in_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=1, is_pointer=True,
                                   type_kind=prop.type_info.type_kind)
            elif is_string:
                in_type = TypeInfo(base_type="IDasReadOnlyString", pointer_level=1, is_pointer=True,
                                   type_kind=prop.type_info.type_kind)
            else:
                in_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=0,
                                   type_kind=prop.type_info.type_kind)
            params = [ParameterDef(name="p_value", type_info=in_type, direction=ParamDirection.IN)]
            methods.append(MethodDef(name=f"Set{prop.name}", return_type=TypeInfo(base_type="DasResult", pointer_level=0), parameters=params))

    return methods


# ---------------------------------------------------------------------------
# Import chain resolver — follow import statements instead of scanning dirs
# ---------------------------------------------------------------------------

def resolve_import_chain(document: "IdlDocument", idl_file_path: str) -> dict:
    """Recursively resolve ``import`` statements and parse referenced IDL files.

    Returns a dict mapping absolute file path → parsed ``IdlDocument`` for every
    transitively imported file.  The starting *document* itself is **not** included.
    Circular imports are safely handled — each file is parsed at most once.

    If *document* already carries ``_imported_docs`` (set by a prior call),
    the cached result is returned immediately without re-parsing.

    Args:
        document: The parsed ``IdlDocument`` whose ``.imports`` list is followed.
        idl_file_path: Absolute or relative path of the IDL file that produced
            *document*; used to resolve relative import paths.
    """
    # Fast path: already resolved for this document
    if hasattr(document, '_imported_docs') and document._imported_docs is not None:
        return document._imported_docs

    if not idl_file_path or not document.imports:
        document._imported_docs = {}
        return {}

    base_dir = Path(idl_file_path).resolve().parent
    self_key = str(Path(idl_file_path).resolve())
    parsed: dict[str, "IdlDocument"] = {}

    def _walk(import_list):
        for imp in import_list:
            imp_path = (base_dir / imp.idl_path.strip('"')).resolve()
            key = str(imp_path)
            # Skip self (circular import) and already-parsed files
            if key == self_key or key in parsed or not imp_path.exists():
                continue
            try:
                imp_doc = _das_idl_parser.parse_idl_file(key)
                parsed[key] = imp_doc
                if imp_doc.imports:
                    _walk(imp_doc.imports)
            except Exception:
                pass  # skip unparseable imports

    _walk(document.imports)
    document._imported_docs = parsed
    return parsed
