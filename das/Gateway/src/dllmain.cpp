/**
 * @file dllmain.cpp
 * @brief DllMain entry point and export stub for DasGateway
 *
 * This file provides a minimal DllMain implementation and a dummy exported
 * function to force MSVC linker to generate an import library (.lib).
 *
 * Background:
 *   DasGateway is a SHARED library that previously had no exported symbols.
 *   On Windows, the MSVC linker only generates an import library when at least
 *   one symbol is exported via __declspec(dllexport). Without an import
 * library, the expected output lib/Debug/DasGateway.lib never exists, causing
 * Ninja to rebuild DasGateway.dll on every invocation, which then triggers
 * DasAutoCopyDll.
 *
 * The `unused` export function serves as a workaround to ensure the import
 * library is generated, thus breaking the unnecessary rebuild chain.
 */

#ifdef _WIN32

#include <windows.h>

#define DAS_GATEWAY_API __declspec(dllexport)

/**
 * @brief Dummy exported function to force import library generation
 *
 * This function intentionally does nothing and is never called.
 * It exists solely to work around the MSVC linker behavior where
 * no import library is generated when a DLL has zero exports.
 *
 * See the file header for the full explanation of why this is needed.
 *
 * @return Always returns 0
 */
extern "C" DAS_GATEWAY_API int unused() { return 0; }

/**
 * @brief DLL entry point
 */
BOOL APIENTRY
DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

#endif // _WIN32
