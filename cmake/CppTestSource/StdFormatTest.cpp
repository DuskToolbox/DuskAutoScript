#include <format>
#include <iostream>

namespace Test
{
    struct CustomType
    {
        int value;
    };
} // namespace Test

template <>
struct std::formatter<::Test::CustomType, char>
    : public formatter<std::string, char>
{
    auto format(const ::Test::CustomType& test_impl, format_context& ctx) const
        -> typename std::remove_reference_t<decltype(ctx)>::iterator
    {
        return std::format_to(ctx.out(), "{}", test_impl.value);
    }
};

int main()
{
    std::cout << std::format("Hello std format in C++ {}", 20) << std::endl;
    std::cout << std::format("Calling test formatter {}", Test::CustomType{11});
}
