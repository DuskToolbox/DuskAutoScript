#include "DasOrt.h"

DAS_CORE_ORTWRAPPER_NS_BEGIN

class PPOcr : public DasOrt
{
    using Base = DasOrt;
    Ort::Session session_;

public:
    PPOcr();
};

DAS_CORE_ORTWRAPPER_NS_END