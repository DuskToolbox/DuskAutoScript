#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAuthoringSession,
    0x69f20003,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    class MaapiAuthoringSession final
        : public PluginInterface::DasTaskAuthoringSessionImplBase<
              MaapiAuthoringSession>
    {
    public:
        MaapiAuthoringSession() = default;
        explicit MaapiAuthoringSession(AcceptedSettingsDto settings);

        DAS_IMPL GetDocument(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_document_json) override;
        DAS_IMPL ApplyChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;
        DAS_IMPL Compile(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

    private:
        AcceptedSettingsDto settings_{};
        int64_t             revision_ = 0;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H
