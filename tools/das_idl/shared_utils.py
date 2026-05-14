"""Shared utility functions for das_idl code generators.

Contains pure functions extracted from multiple generators to avoid duplication.
"""

from pathlib import Path


def idl_path_to_header_name(idl_path: str) -> str:
    """Derive ABI header filename from an IDL file path.

    ``DasJson.idl`` → ``DasJson.h``
    ``IDasImage.idl`` → ``IDasImage.h``
    """
    return Path(idl_path).stem + ".h"


def to_upper_snake(name: str) -> str:
    """Convert PascalCase (or camelCase) to UPPER_SNAKE_CASE.

    Examples:
        ``DasResult`` → ``DAS_RESULT``
        ``MyClass``   → ``MY_CLASS``
        ``IDasAI``    → ``I_DAS_AI``  (consecutive uppercase kept together)
    """
    import re
    # Handle consecutive uppercase followed by uppercase+lowercase (e.g., "AIModel" -> "AI_Model")
    name = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    # Handle lowercase/digit followed by uppercase (e.g., "getResult" -> "get_Result")
    name = re.sub(r'([a-z\d])([A-Z])', r'\1_\2', name)
    return name.upper()


def type_simple_name(type_info) -> str:
    """Return the identifier-safe simple name for a TypeInfo-like object."""
    return getattr(type_info, "simple_name", "") or type_info.base_type.split("::")[-1]


def type_source_name(type_info) -> str:
    """Return the source spelling for a TypeInfo-like object."""
    return getattr(type_info, "source_type", "") or type_info.base_type


def type_resolved_namespace(type_info, get_type_namespace=None) -> str | None:
    """Return the resolved namespace without inventing a fallback.

    ``None`` means the type is not known to the supplied resolver. Empty string
    means a known global-namespace symbol.
    """
    resolved = getattr(type_info, "resolved_namespace", None)
    if resolved is not None and (
        resolved != "" or getattr(type_info, "resolved_qualified_name", "")
    ):
        return resolved
    explicit = getattr(type_info, "explicit_namespace", "")
    if explicit:
        return explicit
    if get_type_namespace is None:
        return None
    return get_type_namespace(type_simple_name(type_info))


def build_param_signatures(
    method,
    get_type_namespace,
    current_namespace,
    type_kind_interface,
    *,
    include_const: bool = False,
    include_reference: bool = False,
) -> tuple[list[str], list[str]]:
    """Build parameter signature lists for SWIG %ignore directives.

    Generates two lists — with and without ``::`` namespace prefix — so that
    callers can emit both the qualified and unqualified SWIG %ignore lines.

    Args:
        method: A *MethodDef*-like object with ``parameters`` iterable.
        get_type_namespace: Callable mapping ``type_name`` → namespace string
            (or ``None``/empty for global namespace).
        current_namespace: The namespace of the owning interface.
        type_kind_interface: The ``TypeKind.INTERFACE`` enum value, used to
            decide whether to add ``::`` prefix for global-namespace types.
        include_const: When *True*, prepend ``const`` for parameters whose
            ``type_info.is_const`` is set.
        include_reference: When *True*, handle ``type_info.is_reference``
            (``&`` suffix) in addition to pointer handling.

    Returns:
        ``(param_signatures_with_prefix, param_signatures_without_prefix)``
    """
    param_signatures_with_prefix: list[str] = []
    param_signatures_without_prefix: list[str] = []

    for param in method.parameters:
        param_type = type_simple_name(param.type_info)
        param_type_with_prefix = param_type

        namespace = type_resolved_namespace(param.type_info, get_type_namespace)
        if not namespace and param.type_info.type_kind == type_kind_interface:
            param_type_with_prefix = f'::{param_type}'
        elif namespace and namespace != current_namespace:
            param_type_with_prefix = f'::{namespace}::{param_type}'

        if param.type_info.is_pointer:
            stars = '*' * param.type_info.pointer_level
            const_prefix = ""
            if include_const and param.type_info.is_const:
                const_prefix = "const "
            param_signatures_with_prefix.append(
                f"{const_prefix}{param_type_with_prefix}{stars}"
            )
            param_signatures_without_prefix.append(
                f"{const_prefix}{param_type}{stars}"
            )
        elif include_reference and param.type_info.is_reference:
            if param.type_info.is_const:
                param_signatures_with_prefix.append(
                    f"const {param_type_with_prefix}&"
                )
                param_signatures_without_prefix.append(
                    f"const {param_type}&"
                )
            else:
                param_signatures_with_prefix.append(
                    f"{param_type_with_prefix}&"
                )
                param_signatures_without_prefix.append(
                    f"{param_type}&"
                )
        else:
            param_signatures_with_prefix.append(param_type_with_prefix)
            param_signatures_without_prefix.append(param_type)

    return param_signatures_with_prefix, param_signatures_without_prefix
