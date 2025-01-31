#include <cwchar>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Utils/StringUtils.h>
#include <gtest/gtest.h>
#include <iostream>
#include <unicode/ustring.h>

void DumpBuffer(const char16_t* p_buffer, size_t size)
{
    std::cout << '\n'
              << "---------DUMP BUFFER FOR POINTER "
              << static_cast<const void*>(p_buffer) << " BEGIN---------"
              << '\n';
    for (size_t i = 0; i < size; ++i)
    {
        const auto buffer_value = static_cast<uint16_t>(p_buffer[i]);
        std::cout << buffer_value << ' ';
    }
    std::cout << '\n';
    std::cout << "---------DUMP BUFFER FOR POINTER "
              << static_cast<const void*>(p_buffer) << " END---------" << '\n';
}

TEST(ICUTest, u_strFromWCSTest1)
{
    int32_t                               size{};
    UErrorCode                            error_code{};
    DAS::Details::DynamicBuffer<char16_t> buffer{};
    char16_t*                             p_buffer{};
    bool                                  is_same{false};

    error_code = U_ZERO_ERROR;
    size = 0;
    const auto& w_string = L"测试";
    const auto  w_string_length = ::wcslen(w_string);
    std::cout << "Calling function \"u_strFromWCS\".\nReturn value ="
              << static_cast<void*>(
                     u_strFromWCS(nullptr, 0, &size, w_string, -1, &error_code))
              << ", size = " << size << ", error_code = " << error_code << '.'
              << '\n';
    std::cout << "NOTE: The input string length which computed by wcslen is "
              << w_string_length << '.' << '\n';
    EXPECT_EQ(size, 2);
    EXPECT_EQ(error_code, U_BUFFER_OVERFLOW_ERROR);

    error_code = U_ZERO_ERROR;
    p_buffer = buffer.DiscardAndGetNullTerminateBufferPointer(size);
    DumpBuffer(p_buffer, buffer.GetSize());
    // 故意设为2，实际需要3，此时应当出现警告。
    u_strFromWCS(
        p_buffer,
        2,
        nullptr,
        w_string,
        static_cast<int32_t>(w_string_length),
        &error_code);
    EXPECT_EQ(error_code, U_STRING_NOT_TERMINATED_WARNING);
    DumpBuffer(p_buffer, buffer.GetSize());
    error_code = U_ZERO_ERROR;
    is_same = U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8(
                  DAS_UTILS_STRINGUTILS_DEFINE_U8STR("测试"))
              == U_NAMESPACE_QUALIFIER UnicodeString{
                  p_buffer,
                  size,
                  static_cast<int32_t>(buffer.GetSize())};
    EXPECT_TRUE(is_same);
    is_same = false;
    // NOTE: destCapacity必须包含字符串尾部的空字符。
    u_strFromWCS(
        p_buffer,
        static_cast<int32_t>(buffer.GetSize()),
        nullptr,
        w_string,
        static_cast<int32_t>(w_string_length),
        &error_code);
    EXPECT_EQ(error_code, U_ZERO_ERROR);
    DumpBuffer(p_buffer, buffer.GetSize());

    is_same = U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8(
                  DAS_UTILS_STRINGUTILS_DEFINE_U8STR("测试"))
              == U_NAMESPACE_QUALIFIER UnicodeString{
                  p_buffer,
                  size,
                  static_cast<int32_t>(buffer.GetSize())};
    EXPECT_TRUE(is_same);
    DumpBuffer(p_buffer, buffer.GetSize());
}

TEST(ICUTest, u_strFromWCSTest2)
{
    int32_t                               size{};
    UErrorCode                            error_code{};
    DAS::Details::DynamicBuffer<char16_t> buffer{};
    char16_t*                             p_buffer{};
    bool                                  is_same{false};

    error_code = U_ZERO_ERROR;
    size = 0;
    const auto& w_string1 = L"这里有两个字：\U00024B62\U0002F805";
    const auto  w_string1_length = ::wcslen(w_string1);
    std::cout
        << "Calling function \"u_strFromWCS\".\nReturn value ="
        << static_cast<void*>(
               u_strFromWCS(nullptr, 0, &size, w_string1, -1, &error_code))
        << ", size = " << size << ", error_code = " << error_code << '.'
        << '\n';
    std::cout << "NOTE: The input string length which computed by wcslen is "
              << w_string1_length << '.' << '\n';
    EXPECT_EQ(size, 11);
    EXPECT_EQ(error_code, U_BUFFER_OVERFLOW_ERROR);

    error_code = U_ZERO_ERROR;
    p_buffer = buffer.DiscardAndGetNullTerminateBufferPointer(size);
    DumpBuffer(p_buffer, buffer.GetSize());
    u_strFromWCS(
        p_buffer,
        static_cast<int32_t>(buffer.GetSize()),
        nullptr,
        w_string1,
        static_cast<int32_t>(w_string1_length),
        &error_code);
    EXPECT_EQ(error_code, U_ZERO_ERROR);
    DumpBuffer(p_buffer, buffer.GetSize());

    is_same = U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8(
                  DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
                      "这里有两个字：\U00024B62\U0002F805"))
              == U_NAMESPACE_QUALIFIER UnicodeString{
                  p_buffer,
                  size,
                  static_cast<int32_t>(buffer.GetSize())};
    EXPECT_TRUE(is_same);
    DumpBuffer(p_buffer, buffer.GetSize());

    /**
     * The buffer contents is (probably) not NUL-terminated.
     * You can check if it is with (s.length() < s.getCapacity() &&
     * buffer[s.length()]==0). (See getTerminatedBuffer().)
     */
    const auto icu_string = U_NAMESPACE_QUALIFIER UnicodeString{
        p_buffer,
        size,
        static_cast<int32_t>(buffer.GetSize())};
    EXPECT_EQ(icu_string.length(), 11);
    EXPECT_EQ(icu_string.countChar32(), 9);
    EXPECT_EQ(icu_string.getCapacity(), 12);
    EXPECT_TRUE(
        (icu_string.length() < icu_string.getCapacity()
         && icu_string.getBuffer()[icu_string.length()] == 0));
}

TEST(ICUTest, UnicodeStringToUtf32Test)
{
    const auto icu_string = U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8(
        DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
            "这里有两个字：\U00024B62\U0002F805"));
    EXPECT_EQ(icu_string.length(), 11);
    EXPECT_EQ(icu_string.countChar32(), 9);
    std::cout << "NOTE: icu_string.getCapacity() = " << icu_string.getCapacity()
              << '.' << '\n';
}