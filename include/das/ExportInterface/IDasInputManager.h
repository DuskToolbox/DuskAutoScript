#ifndef DAS_DASINPUTMANAGER_H
#define DAS_DASINPUTMANAGER_H

#include <das/PluginInterface/IDasInput.h>

// {53D6A1A0-FB5B-46BC-8108-3D98095AF2B9}
DAS_DEFINE_GUID(
    DAS_IID_INPUT_MANANGER,
    IDasInputManager,
    0x53d6a1a0,
    0xfb5b,
    0x46bc,
    0x81,
    0x8,
    0x3d,
    0x98,
    0x9,
    0x5a,
    0xf2,
    0xb9);
SWIG_IGNORE(IDasInputManager)
DAS_INTERFACE IDasInputManager{

};

#endif // DAS_DASINPUTMANAGER_H
