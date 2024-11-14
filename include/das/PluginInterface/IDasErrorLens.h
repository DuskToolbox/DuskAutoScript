#ifndef DAS_IERRORLENS_H
#define DAS_IERRORLENS_H

#include <das/IDasTypeInfo.h>
#include <das/ExportInterface/IDasGuidVector.h>
#include <das/DasString.hpp>

// {10963BC9-72FD-4B57-A7BA-98F431C234E4}
DAS_DEFINE_GUID(
    DAS_IID_ERROR_LENS,
    IDasErrorLens,
    0x10963bc9,
    0x72fd,
    0x4b57,
    0xa7,
    0xba,
    0x98,
    0xf4,
    0x31,
    0xc2,
    0x34,
    0xe4);
SWIG_IGNORE(IDasErrorLens)
DAS_INTERFACE IDasErrorLens : public IDasBase
{
    DAS_METHOD GetSupportedIids(IDasReadOnlyGuidVector * *pp_out_iids) = 0;
    DAS_METHOD GetErrorMessage(
        IDasReadOnlyString * locale_name,
        DasResult error_code,
        IDasReadOnlyString * *out_string) = 0;
};

// {0B9B86B2-F8A6-4EA4-90CF-3756AD318B83}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_ERROR_LENS,
    IDasSwigErrorLens,
    0xb9b86b2,
    0xf8a6,
    0x4ea4,
    0x90,
    0xcf,
    0x37,
    0x56,
    0xad,
    0x31,
    0x8b,
    0x83);
DAS_SWIG_DIRECTOR_ATTRIBUTE(IDasSwigErrorLens)
DAS_INTERFACE IDasSwigErrorLens : public IDasSwigBase
{
    virtual DasRetReadOnlyGuidVector GetSupportedIids() = 0;
    virtual DasRetReadOnlyString GetErrorMessage(
        const DasReadOnlyString locale_name,
        DasResult               error_code) = 0;
};

SWIG_IGNORE(DasGetErrorMessage)
/**
 * @brief Get the error explanation. If return value is not DAS_S_OK,
        then the pp_out_error_explanation points to a string
        that explains why calling this function failed
 * @param p_error_generating_interface_iid
 * @param locale_name
 * @param error_code
 * @param pp_out_error_explanation It is an Error message when this function
  success. Otherwise it is a string that explains why calling this function
  failed.
 * @return DasResult
 */
DAS_C_API DasResult DasGetErrorMessage(
    IDasTypeInfo*        p_error_generator,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message);

SWIG_IGNORE(DasGetPredefinedErrorMessage)
DAS_C_API DasResult DasGetPredefinedErrorMessage(
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message);

/**
 * @brief See DasGetErrorMessage
 *
 * @param p_error_generating_interface_iid
 * @param locale_name
 * @param error_code
 * @return DasRetReadOnlyString
 */
DAS_API DasRetReadOnlyString
DasGetErrorMessage(IDasSwigTypeInfo* p_error_generator, DasResult error_code);

DAS_API DasRetReadOnlyString DasGetPredefinedErrorMessage(DasResult error_code);

#ifndef SWIG
/**
 * @brief Always return DAS_S_OK
 */
DAS_C_API DasResult DasSetDefaultLocale(IDasReadOnlyString* locale_name);
/**
 * @brief See SetDefaultLocale
 */
DAS_C_API DasResult DasGetDefaultLocale(IDasReadOnlyString** locale_name);
#endif // SWIG

DAS_API DasResult            DasSetDefaultLocale(DasReadOnlyString locale_name);
DAS_API DasRetReadOnlyString DasGetDefaultLocale();

#endif // DAS_IERRORLENS_H
