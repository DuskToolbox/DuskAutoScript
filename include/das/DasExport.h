#ifndef DAS_EXPORT_H
#define DAS_EXPORT_H

#if (defined _WIN32 || defined __CYGWIN__)
#define DAS_DLL_IMPORT __declspec(dllimport)
#define DAS_DLL_EXPORT __declspec(dllexport)
#define DAS_DLL_LOCAL
#elif __GNUC__ >= 4
#define DAS_DLL_IMPORT __attribute__((visibility("default")))
#define DAS_DLL_EXPORT __attribute__((visibility("default")))
#define DAS_DLL_LOCAL __attribute__((visibility("hidden")))
#else
#define DAS_DLL_IMPORT
#define DAS_DLL_EXPORT
#define DAS_DLL_LOCAL
#endif

#ifdef DAS_BUILD_SHARED
#define DAS_EXPORT DAS_DLL_EXPORT
#else
#define DAS_EXPORT DAS_DLL_IMPORT
#endif

#ifdef __cplusplus
#define DAS_EXTERN_C extern "C"
#else
#define DAS_EXTERN_C
#endif

#define DAS_C_API DAS_EXTERN_C DAS_EXPORT
/**
 * @brief
 * 使用此宏定义的接口，不具有且未来也不计划具有ABI兼容性，使用DasCore的用户也不应该调用这些接口
 */
#define DAS_API DAS_DLL_EXPORT

#ifdef _MSC_VER
#define DAS_FUNCTION __FUNCTION__
#else
#define DAS_FUNCTION __PRETTY_FUNCTION__
#endif // _MSC_VER

#endif // DAS_EXPORT_H
