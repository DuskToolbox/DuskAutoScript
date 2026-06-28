#ifndef DAS_CORE_GRAPHRUNTIME_PORTFRAME_H
#define DAS_CORE_GRAPHRUNTIME_PORTFRAME_H

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/DasPtr.hpp>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// PortValueType — discriminates the 10 runtime value kinds
// ---------------------------------------------------------------------------
enum class PortValueType : uint8_t
{
    Null = 0,
    Int = 1,
    Float = 2,
    String = 3,
    Bool = 4,
    Base = 5,
    Component = 6,
    Image = 7,
    Json = 8,
    Signal = 9
};

// ---------------------------------------------------------------------------
// Wrapper types so distinct semantic roles survive std::variant dedup
// ---------------------------------------------------------------------------
struct BaseHandle
{
    DAS::DasPtr<IDasBase> ptr;
};

struct ComponentHandle
{
    DAS::DasPtr<IDasBase> ptr;
};

struct ImageData
{
    std::vector<uint8_t> bytes;
};

struct JsonData
{
    yyjson::value value;
};

struct SignalTag
{
};

// ---------------------------------------------------------------------------
// PortValue — type-erased, 10-kind runtime value
// ---------------------------------------------------------------------------
class PortValue
{
public:
    using Storage = std::variant<
        std::monostate,  // Null
        int64_t,         // Int
        double,          // Float
        std::string,     // String
        bool,            // Bool
        BaseHandle,      // Base
        ComponentHandle, // Component
        ImageData,       // Image
        JsonData,        // Json
        SignalTag        // Signal
        >;

    // -- Null ----------------------------------------------------------------
    PortValue() = default;

    // -- Scalar constructors -------------------------------------------------
    explicit PortValue(int64_t v);
    explicit PortValue(double v);
    explicit PortValue(std::string v);
    explicit PortValue(const char* v);
    explicit PortValue(bool v);

    // -- COM-handle constructors ---------------------------------------------
    explicit PortValue(BaseHandle v);
    explicit PortValue(ComponentHandle v);

    // -- Data constructors ---------------------------------------------------
    explicit PortValue(ImageData v);
    explicit PortValue(JsonData v);

    // -- Signal factory ------------------------------------------------------
    static PortValue Signal();

    // -- Observers -----------------------------------------------------------
    [[nodiscard]]
    PortValueType GetType() const noexcept;

    [[nodiscard]]
    bool IsNull() const noexcept;
    [[nodiscard]]
    bool IsInt() const noexcept;
    [[nodiscard]]
    bool IsFloat() const noexcept;
    [[nodiscard]]
    bool IsString() const noexcept;
    [[nodiscard]]
    bool IsBool() const noexcept;
    [[nodiscard]]
    bool IsBase() const noexcept;
    [[nodiscard]]
    bool IsComponent() const noexcept;
    [[nodiscard]]
    bool IsImage() const noexcept;
    [[nodiscard]]
    bool IsJson() const noexcept;
    [[nodiscard]]
    bool IsSignal() const noexcept;

    // -- Checked accessors (return nullptr on type mismatch) ------------------
    [[nodiscard]]
    const int64_t* AsInt() const noexcept;
    [[nodiscard]]
    const double* AsFloat() const noexcept;
    [[nodiscard]]
    const std::string* AsString() const noexcept;
    [[nodiscard]]
    const bool* AsBool() const noexcept;
    [[nodiscard]]
    const BaseHandle* AsBase() const noexcept;
    [[nodiscard]]
    const ComponentHandle* AsComponent() const noexcept;
    [[nodiscard]]
    const ImageData* AsImage() const noexcept;
    [[nodiscard]]
    const JsonData* AsJson() const noexcept;

    // -- Direct variant access -----------------------------------------------
    [[nodiscard]]
    const Storage& GetStorage() const noexcept;

private:
    Storage storage_;
};

// ---------------------------------------------------------------------------
// PortKey — composite key (node_id, port_id) for the frame map
// ---------------------------------------------------------------------------
struct PortKey
{
    DasGuid     node_id{};
    std::string port_id;

    bool operator==(const PortKey& other) const noexcept;
    bool operator!=(const PortKey& other) const noexcept;
};

struct PortKeyHash
{
    std::size_t operator()(const PortKey& key) const noexcept;
};

// ---------------------------------------------------------------------------
// PortFrame — unordered_map<DasGuid+port_id, PortValue>
//
// A snapshot of all port values for a compiled graph at a single execution
// step.  Core-internal only — no IDL, no IPC.
// ---------------------------------------------------------------------------
class PortFrame
{
public:
    using MapType = std::unordered_map<PortKey, PortValue, PortKeyHash>;

    // -- Mutators ------------------------------------------------------------
    void Set(const PortKey& key, PortValue value);
    void Set(DasGuid node_id, std::string port_id, PortValue value);

    [[nodiscard]]
    bool Remove(const PortKey& key);
    void Clear() noexcept;

    // Drop every Signal-typed entry owned by @p node_id. Used by the signal-gated
    // runtime at loop iteration boundaries to clear stale control markers
    // (e.g. a branch's true/false output from the previous iteration) so gates
    // re-evaluate cleanly. Data entries are preserved. (DAS-60 Stage 3.)
    std::size_t ClearSignalsByNode(DasGuid node_id);

    // -- Observers -----------------------------------------------------------
    [[nodiscard]]
    const PortValue* Find(const PortKey& key) const noexcept;
    [[nodiscard]]
    bool Contains(const PortKey& key) const noexcept;
    [[nodiscard]]
    std::size_t Size() const noexcept;
    [[nodiscard]]
    bool Empty() const noexcept;

    // -- Iteration -----------------------------------------------------------
    [[nodiscard]]
    MapType::const_iterator begin() const noexcept;
    [[nodiscard]]
    MapType::const_iterator end() const noexcept;

private:
    MapType entries_;
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_PORTFRAME_H
