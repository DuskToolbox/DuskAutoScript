#ifndef ASR_IINPUT_H
#define ASR_IINPUT_H

#include <AutoStarRail/IAsrTypeInfo.h>

typedef enum AsrClickType
{
    ASR_CLICK_LEFT = 1,
    ASR_CLICK_RIGHT = 2,
    ASR_CLICK_MIDDLE = 4
} AsrClickType;

typedef struct _asr_Point
{
    int32_t x;
    int32_t y;
} AsrPoint;

// {02F6A16A-01FD-4303-886A-9B60373EBE8C}
ASR_DEFINE_GUID(
    ASR_IID_INPUT,
    IAsrInput,
    0x2f6a16a,
    0x1fd,
    0x4303,
    0x88,
    0x6a,
    0x9b,
    0x60,
    0x37,
    0x3e,
    0xbe,
    0x8c);
SWIG_IGNORE(IAsrInput)
ASR_INTERFACE IAsrInput : public IAsrTypeInfo
{
    /**
     * @brief If the implementation is a mouse, then the function assumes that
     * the left button is pressed.
     *
     * @param x
     * @param y
     * @return ASR_METHOD
     */
    ASR_METHOD Click(int32_t x, int32_t y) = 0;
};

// {DDB17BB3-E6B2-4FD8-8E06-C037EEF18D65}
ASR_DEFINE_GUID(
    ASR_IID_TOUCH,
    IAsrTouch,
    0xddb17bb3,
    0xe6b2,
    0x4fd8,
    0x8e,
    0x6,
    0xc0,
    0x37,
    0xee,
    0xf1,
    0x8d,
    0x65);
SWIG_IGNORE(IAsrTouch)
ASR_INTERFACE IAsrTouch : public IAsrInput
{
    ASR_METHOD Swipe(AsrPoint from, AsrPoint to, int32_t duration_ms) = 0;
};

// {36344E73-14D9-4FDB-ADDF-F5FB161DB813}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_INPUT,
    IAsrSwigInput,
    0x36344e73,
    0x14d9,
    0x4fdb,
    0xad,
    0xdf,
    0xf5,
    0xfb,
    0x16,
    0x1d,
    0xb8,
    0x13);
ASR_SWIG_DIRECTOR_ATTRIBUTE(IAsrSwigInput)
ASR_INTERFACE IAsrSwigInput : public IAsrSwigTypeInfo
{
    ASR_METHOD Click(int32_t x, int32_t y) = 0;
};

// {5A151018-4347-4613-B748-17B03B0598EC}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_TOUCH,
    IAsrSwigTouch,
    0x5a151018,
    0x4347,
    0x4613,
    0xb7,
    0x48,
    0x17,
    0xb0,
    0x3b,
    0x5,
    0x98,
    0xec);
ASR_SWIG_DIRECTOR_ATTRIBUTE(IAsrSwigTouch)
ASR_INTERFACE IAsrSwigTouch : public IAsrSwigInput
{
    ASR_METHOD Swipe(AsrPoint from, AsrPoint to, int32_t duration_ms) = 0;
};

ASR_DEFINE_RET_POINTER(AsrRetInput, IAsrSwigInput);

// {B03F0BB5-B328-45C4-99D1-04DBC7FC5BA7}
ASR_DEFINE_GUID(
    ASR_IID_TOUCH_FACTORY,
    IAsrInputFactory,
    0xb03f0bb5,
    0xb328,
    0x45c4,
    0x99,
    0xd1,
    0x4,
    0xdb,
    0xc7,
    0xfc,
    0x5b,
    0xa7);
SWIG_IGNORE(IAsrInputFactory)
ASR_INTERFACE IAsrInputFactory : public IAsrTypeInfo
{
    ASR_METHOD CreateInstance(
        IAsrReadOnlyString * p_json_config,
        IAsrInput * *pp_out_input) = 0;
};

// {D96C059A-BBE8-4614-8D22-A0B035C40795}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_TOUCH_FACTORY,
    IAsrSwigInputFactory,
    0xd96c059a,
    0xbbe8,
    0x4614,
    0x8d,
    0x22,
    0xa0,
    0xb0,
    0x35,
    0xc4,
    0x7,
    0x95);
ASR_SWIG_EXPORT_ATTRIBUTE(IAsrSwigInputFactory)
ASR_INTERFACE IAsrSwigInputFactory : public IAsrSwigTypeInfo
{
    virtual AsrRetInput CreateInstance(AsrReadOnlyString json_config) = 0;
};

ASR_DEFINE_RET_POINTER(AsrRetInputFactory, IAsrSwigInputFactory);

#endif // ASR_IINPUT_H
