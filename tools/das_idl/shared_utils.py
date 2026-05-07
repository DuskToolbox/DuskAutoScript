"""Shared utility functions for das_idl code generators.

Contains pure functions extracted from multiple generators to avoid duplication.
"""


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
        param_type = param.type_info.base_type
        param_type_with_prefix = param_type

        namespace = get_type_namespace(param_type)
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
