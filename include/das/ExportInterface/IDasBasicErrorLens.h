#ifndef DAS_BASICERRORLENS_H
#define DAS_BASICERRORLENS_H

#include <das/PluginInterface/IDasErrorLens.h>

DAS_INTERFACE IDasErrorLens;
DAS_INTERFACE IDasSwigErrorLens;

// {813FD58D-5632-4A43-A87E-26E412D9EADD}
DAS_DEFINE_GUID(
    DAS_IID_BASIC_ERROR_LENS,
    IDasBasicErrorLens,
    0x813fd58d,
    0x5632,
    0x4a43,
    0xa8,
    0x7e,
    0x26,
    0xe4,
    0x12,
    0xd9,
    0xea,
    0xdd);
SWIG_IGNORE(IDasBasicErrorLens)
/**
 * @brief A basic error lens implementation for developers.
 */
DAS_INTERFACE IDasBasicErrorLens : public IDasErrorLens
{
    DAS_METHOD RegisterErrorMessage(
        IDasReadOnlyString * locale_name,
        DasResult error_code,
        IDasReadOnlyString * p_error_message) = 0;
    DAS_METHOD GetWritableSupportedIids(IDasGuidVector * *pp_out_iids) = 0;
};

// {F44EBCCB-3110-4B0B-BB1A-E0C194E41F9B}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_BASIC_ERROR_LENS,
    IDasSwigBasicErrorLens,
    0xf44ebccb,
    0x3110,
    0x4b0b,
    0xbb,
    0x1a,
    0xe0,
    0xc1,
    0x94,
    0xe4,
    0x1f,
    0x9b);
DAS_SWIG_DIRECTOR_ATTRIBUTE(IDasSwigBasicErrorLens)
DAS_INTERFACE IDasSwigBasicErrorLens : public IDasSwigErrorLens
{
    virtual DasResult RegisterErrorMessage(
        DasReadOnlyString locale_name,
        DasResult         error_code,
        DasReadOnlyString error_message) = 0;
    virtual DasRetGuidVector GetWritableSupportedIids() = 0;
};

DAS_DEFINE_RET_POINTER(DasRetBasicErrorLens, IDasSwigErrorLens);

SWIG_IGNORE(CreateIDasBasicErrorLens)
DAS_C_API DasResult
CreateIDasBasicErrorLens(IDasBasicErrorLens** pp_out_error_lens);

DAS_API DasRetBasicErrorLens CreateIDasSwigBasicErrorLens();

#endif // DAS_BASICERRORLENS_H
