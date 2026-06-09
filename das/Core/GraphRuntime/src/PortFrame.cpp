#include <das/Core/GraphRuntime/PortFrame.h>

#include <cstring>
#include <utility>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ===========================================================================
// PortValue implementation
// ===========================================================================

PortValue::PortValue(const int64_t v) : storage_(v) {}

PortValue::PortValue(const double v) : storage_(v) {}

PortValue::PortValue(std::string v) : storage_(std::move(v)) {}

PortValue::PortValue(const char* v) : storage_(std::string(v)) {}

PortValue::PortValue(const bool v) : storage_(v) {}

PortValue::PortValue(BaseHandle v) : storage_(std::move(v)) {}

PortValue::PortValue(ComponentHandle v) : storage_(std::move(v)) {}

PortValue::PortValue(ImageData v) : storage_(std::move(v)) {}

PortValue::PortValue(JsonData v) : storage_(std::move(v)) {}

PortValue PortValue::Signal()
{
    PortValue pv;
    pv.storage_ = SignalTag{};
    return pv;
}

PortValueType PortValue::GetType() const noexcept
{
    return static_cast<PortValueType>(storage_.index());
}

bool PortValue::IsNull() const noexcept
{
    return std::holds_alternative<std::monostate>(storage_);
}

bool PortValue::IsInt() const noexcept
{
    return std::holds_alternative<int64_t>(storage_);
}

bool PortValue::IsFloat() const noexcept
{
    return std::holds_alternative<double>(storage_);
}

bool PortValue::IsString() const noexcept
{
    return std::holds_alternative<std::string>(storage_);
}

bool PortValue::IsBool() const noexcept
{
    return std::holds_alternative<bool>(storage_);
}

bool PortValue::IsBase() const noexcept
{
    return std::holds_alternative<BaseHandle>(storage_);
}

bool PortValue::IsComponent() const noexcept
{
    return std::holds_alternative<ComponentHandle>(storage_);
}

bool PortValue::IsImage() const noexcept
{
    return std::holds_alternative<ImageData>(storage_);
}

bool PortValue::IsJson() const noexcept
{
    return std::holds_alternative<JsonData>(storage_);
}

bool PortValue::IsSignal() const noexcept
{
    return std::holds_alternative<SignalTag>(storage_);
}

const int64_t* PortValue::AsInt() const noexcept
{
    return std::get_if<int64_t>(&storage_);
}

const double* PortValue::AsFloat() const noexcept
{
    return std::get_if<double>(&storage_);
}

const std::string* PortValue::AsString() const noexcept
{
    return std::get_if<std::string>(&storage_);
}

const bool* PortValue::AsBool() const noexcept
{
    return std::get_if<bool>(&storage_);
}

const BaseHandle* PortValue::AsBase() const noexcept
{
    return std::get_if<BaseHandle>(&storage_);
}

const ComponentHandle* PortValue::AsComponent() const noexcept
{
    return std::get_if<ComponentHandle>(&storage_);
}

const ImageData* PortValue::AsImage() const noexcept
{
    return std::get_if<ImageData>(&storage_);
}

const JsonData* PortValue::AsJson() const noexcept
{
    return std::get_if<JsonData>(&storage_);
}

const PortValue::Storage& PortValue::GetStorage() const noexcept
{
    return storage_;
}

// ===========================================================================
// PortKey
// ===========================================================================

bool PortKey::operator==(const PortKey& other) const noexcept
{
    return node_id == other.node_id && port_id == other.port_id;
}

bool PortKey::operator!=(const PortKey& other) const noexcept
{
    return !(*this == other);
}

// ===========================================================================
// PortKeyHash
// ===========================================================================

std::size_t PortKeyHash::operator()(const PortKey& key) const noexcept
{
    auto h1 = std::hash<DasGuid>{}(key.node_id);
    auto h2 = std::hash<std::string>{}(key.port_id);
    h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
    return h1;
}

// ===========================================================================
// PortFrame
// ===========================================================================

void PortFrame::Set(const PortKey& key, PortValue value)
{
    entries_.insert_or_assign(key, std::move(value));
}

void PortFrame::Set(DasGuid node_id, std::string port_id, PortValue value)
{
    Set(PortKey{node_id, std::move(port_id)}, std::move(value));
}

bool PortFrame::Remove(const PortKey& key) { return entries_.erase(key) > 0; }

void PortFrame::Clear() noexcept { entries_.clear(); }

const PortValue* PortFrame::Find(const PortKey& key) const noexcept
{
    auto it = entries_.find(key);
    return it != entries_.end() ? &it->second : nullptr;
}

bool PortFrame::Contains(const PortKey& key) const noexcept
{
    return entries_.count(key) > 0;
}

std::size_t PortFrame::Size() const noexcept { return entries_.size(); }

bool PortFrame::Empty() const noexcept { return entries_.empty(); }

PortFrame::MapType::const_iterator PortFrame::begin() const noexcept
{
    return entries_.cbegin();
}

PortFrame::MapType::const_iterator PortFrame::end() const noexcept
{
    return entries_.cend();
}

DAS_CORE_GRAPHRUNTIME_NS_END
