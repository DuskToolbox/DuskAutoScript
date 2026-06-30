#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKAUTHORINGSESSION_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKAUTHORINGSESSION_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>

#include <cstdint>
#include <string>

// Forward declaration of the Core-owned opaque authoring session state (defined
// in GraphRuntimeFactory.cpp). Global scope — matches the C ABI declaration in
// GraphRuntimeFactory.h.
struct GraphAuthoringSessionState;

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskAuthoringSession,
    0x1E3FBC78,
    0x0B9B,
    0x40BF,
    0xB1,
    0xA4,
    0x18,
    0x64,
    0xFA,
    0x31,
    0x8F,
    0x83);

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    // IDasTaskAuthoringSession shell (DAS-77). Holds an opaque handle to a
    // Core-owned authoring session whose authoritative store is a
    // GraphDocumentDto (never exposed across the boundary). GetDocument /
    // ApplyChange / Compile delegate to the Core C ABI; the plugin writes no
    // graph logic. The store lives in Core; this object only owns the handle
    // (RAII: DestroyGraphAuthoringSession in the destructor).
    class DasGraphTaskAuthoringSession final
        : public PluginInterface::DasTaskAuthoringSessionImplBase<
              DasGraphTaskAuthoringSession>
    {
    public:
        /// Create a session seeded from a context JSON string. The context may
        /// be a formSequence doc, a graph doc, or empty.
        explicit DasGraphTaskAuthoringSession(std::string context_json);
        ~DasGraphTaskAuthoringSession() override;

        DasGraphTaskAuthoringSession(const DasGraphTaskAuthoringSession&)            = delete;
        DasGraphTaskAuthoringSession& operator=(const DasGraphTaskAuthoringSession&) = delete;

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
        // Opaque Core-owned authoring session (GraphAuthoringSessionState*).
        GraphAuthoringSessionState* state_ = nullptr;
    };
} // namespace Plugins::DasGraphTask
DAS_NS_END

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKAUTHORINGSESSION_H
