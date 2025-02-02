#include <boost/dll.hpp>
#include <das/Utils/ThreadUtils.h>
#include <iostream>

#ifdef DAS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
    using SetThreadDescriptionFunction = HRESULT WINAPI(HANDLE, PCWSTR);
}

#endif // DAS_WINDOWS

DAS_UTILS_NS_BEGIN

void SetCurrentThreadName(const wchar_t* name)
{
#ifdef DAS_WINDOWS
    try
    {
        auto fnSetThreadDescription =
            boost::dll::import_symbol<SetThreadDescriptionFunction>(
                "Kernel32.dll",
                "SetThreadDescription",
                boost::dll::load_mode::search_system_folders);
        fnSetThreadDescription(::GetCurrentThread(), name);
    }
    catch (const boost::dll::fs::system_error& ex)
    {
        const auto& error_code = ex.code();
        std::cout << "Call SetThreadDescription failed." << std::endl;
        std::cout << "What = " << ex.what() << " Code = " << error_code.value()
                  << " Message = " << error_code.message();
    }
#else
    std::ignore = name;
#endif // DAS_WINDOWS
}

DAS_UTILS_NS_END
