#!/usr/bin/env python3
"""
IPC Header Usage Checker

Detects code that bypasses IpcHeaderValidator or uses raw IPCMessageHeader inappropriately.
This script can be run as a pre-commit hook to prevent invalid IPC header usage.

Usage:
    python tools/check_ipc_header_usage.py              # Check all IPC files
    python tools/check_ipc_header_usage.py --staged     # Check only staged files
    python tools/check_ipc_header_usage.py --install-hook # Install pre-commit hook
    python tools/check_ipc_header_usage.py --help        # Show help
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


# Patterns that indicate bypassing the validator
BYPASS_PATTERNS = [
    # Manual magic check
    (r'\.magic\s*==?\s*IPCMessageHeader::MAGIC',
     'Manual magic check - use IpcHeaderValidator::QuickValidate()'),
    (r'\.magic\s*!=\s*IPCMessageHeader::MAGIC',
     'Manual magic check - use IpcHeaderValidator::QuickValidate()'),

    # Manual version check
    (r'\.version\s*==?\s*IPCMessageHeader::CURRENT_VERSION',
     'Manual version check - use IpcHeaderValidator::QuickValidate()'),
    (r'\.version\s*!=\s*IPCMessageHeader::CURRENT_VERSION',
     'Manual version check - use IpcHeaderValidator::QuickValidate()'),

    # Manual message type check
    (r'\.message_type\s*==?\s*MessageType::',
     'Manual message type check - use IpcHeaderValidator::ValidateMessageType()'),

    # Manual control plane check (checking interface_id directly)
    (r'\.interface_id\s*>=?\s*HandshakeInterfaceId::',
     'Manual control plane check - use IpcHeaderValidator::IsControlPlane()'),
]

# Patterns that indicate using raw IPCMessageHeader in function parameters
RAW_HEADER_PARAM_PATTERNS = [
    (r'IPCMessageHeader\s*&\s*\w+\s*\)',
     'Function parameter uses raw IPCMessageHeader - use ValidatedIPCMessageHeader'),
    (r'const\s+IPCMessageHeader\s*&\s*\w+\s*\)',
     'Function parameter uses raw IPCMessageHeader - use ValidatedIPCMessageHeader'),
    (r'IPCMessageHeader\s*\*\s*\w+\s*\)',
     'Function parameter uses raw IPCMessageHeader* - use ValidatedIPCMessageHeader'),
]

# Whitelist - patterns that are allowed
WHITELIST_PATTERNS = [
    # Uses the validator
    r'IpcHeaderValidator::',
    # Uses the validated type
    r'ValidatedIPCMessageHeader',
    # Type definition files
    r'IpcMessageHeader\.h',
    r'IpcHeaderValidator\.h',
    r'IpcMessageHeaderBuilder\.h',
    # IStubBase.h has Dispatch() output parameter exception
    r'IStubBase\.h',
    # NOLINT comment to explicitly bypass
    r'//\s*NOLINT.*ipc-validator',
    r'/\*.*NOLINT.*ipc-validator.*\*/',
]


def get_staged_files():
    """Get list of staged files from git."""
    try:
        result = subprocess.run(
            ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACM'],
            capture_output=True,
            text=True,
            check=True
        )
        return [f.strip() for f in result.stdout.split('\n') if f.strip()]
    except subprocess.CalledProcessError:
        return []


def get_all_ipc_files():
    """Get list of all IPC-related source files."""
    ipc_dir = Path('das/Core/IPC')
    if not ipc_dir.exists():
        return []

    patterns = ['*.cpp', '*.h', '*.hpp']
    files = []
    for pattern in patterns:
        files.extend(ipc_dir.rglob(pattern))

    return [str(f) for f in files if f.is_file()]


def should_skip_file(filepath):
    """Check if file should be skipped (whitelisted locations)."""
    skip_patterns = [
        r'IpcMessageHeader\.h$',
        r'IpcHeaderValidator\.h$',
        r'IpcMessageHeaderBuilder\.h$',
        r'ValidatedIPCMessageHeader\.h$',
        r'IStubBase\.h$',
        # Test files are allowed to use raw IPCMessageHeader for testing header serialization
        r'test[/\\]Ipc.*Test\.cpp$',
        r'test[/\\].*Test\.cpp$',
    ]

    filepath_normalized = filepath.replace('\\', '/')
    for pattern in skip_patterns:
        if re.search(pattern, filepath_normalized):
            return True
    return False


def check_file(filepath):
    """Check a single file for invalid IPC header usage."""
    if should_skip_file(filepath):
        return []

    if not os.path.exists(filepath):
        return []

    errors = []

    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
            lines = content.split('\n')
    except Exception as e:
        return [(filepath, 0, f"Error reading file: {e}")]

    # Check for bypass patterns
    for pattern, message in BYPASS_PATTERNS:
        for i, line in enumerate(lines, 1):
            # Skip comments and strings
            if re.search(r'//.*' + re.escape(pattern.split('=')[0].strip()), line):
                continue
            if re.search(r'"[^"]*' + re.escape(pattern.split('=')[0].strip()) + r'[^"]*"', line):
                continue

            if re.search(pattern, line):
                # Check if whitelisted
                if any(re.search(wp, line) for wp in WHITELIST_PATTERNS):
                    continue
                errors.append((filepath, i, message))

    # Check for raw header in function parameters
    for pattern, message in RAW_HEADER_PARAM_PATTERNS:
        for i, line in enumerate(lines, 1):
            # Skip comments
            if line.strip().startswith('//'):
                continue

            # Check if this is a function definition/declaration
            if re.search(r'^\s*(inline\s+)?(virtual\s+)?(const\s+)?(\w+\s*\*?\s+)+(\w+)\s*\(', line):
                if re.search(pattern, line):
                    # Check if whitelisted
                    if any(re.search(wp, line) for wp in WHITELIST_PATTERNS):
                        continue
                    # Additional check: allow output parameters in certain contexts
                    if 'out_' in line or 'Output' in line:
                        continue
                    errors.append((filepath, i, message))

    return errors


def main():
    parser = argparse.ArgumentParser(
        description='Check for invalid IPC header usage'
    )
    parser.add_argument(
        '--staged',
        action='store_true',
        help='Check only staged files'
    )
    parser.add_argument(
        '--install-hook',
        action='store_true',
        help='Install pre-commit hook'
    )
    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='Show verbose output'
    )

    args = parser.parse_args()

    # Handle --install-hook
    if args.install_hook:
        hook_path = Path('.git/hooks/pre-commit')
        hook_path.parent.mkdir(parents=True, exist_ok=True)

        hook_content = '''#!/bin/sh
# Pre-commit hook for IPC header validation
# Installed by check_ipc_header_usage.py

python tools/check_ipc_header_usage.py --staged
exit $?
'''

        hook_path.write_text(hook_content, encoding='utf-8')
        print(f"Pre-commit hook installed at: {hook_path}")
        print("Run 'chmod +x .git/hooks/pre-commit' to make it executable on Unix")
        return 0

    # Determine which files to check
    if args.staged:
        files = get_staged_files()
        # Filter to IPC files only
        files = [f for f in files if 'das/Core/IPC' in f.replace('\\', '/')]
    else:
        files = get_all_ipc_files()

    if not files:
        print("No IPC files to check")
        return 0

    if args.verbose:
        print(f"Checking {len(files)} files...")

    # Check all files
    all_errors = []
    for filepath in files:
        errors = check_file(filepath)
        all_errors.extend(errors)

    # Report results
    if all_errors:
        print("ERROR: Invalid IPC header usage detected!")
        print()

        for filepath, line, message in all_errors:
            print(f"  {filepath}:{line}")
            print(f"    {message}")
            print()

        print("To fix:")
        print("  1. Use IpcHeaderValidator::QuickValidate() instead of manual checks")
        print("  2. Use ValidatedIPCMessageHeader instead of IPCMessageHeader")
        print("  3. Add '// NOLINT:ipc-validator' to explicitly bypass (use sparingly)")
        print()
        return 1
    else:
        if args.verbose:
            print("All IPC files passed validation")
        return 0


if __name__ == '__main__':
    sys.exit(main())
