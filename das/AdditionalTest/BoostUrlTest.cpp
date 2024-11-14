#include <boost/url.hpp>
#include <gtest/gtest.h>
#include <iostream>

TEST(BoostImplementationCheck, UrlExceptionTest)
{
    // Will not throw exceptions.
    boost::url_view test{""};
    // Use empty value to indicate failure.
    EXPECT_EQ(test.scheme(), std::string_view{});

    test = {"adb"};
    EXPECT_EQ(test.scheme(), std::string_view{});

    test = {"adb://"};
    EXPECT_EQ(test.scheme(), std::string_view{"adb"});
}

TEST(BoostImplementationCheck, UrlPartlyResolvedTest)
{
    EXPECT_THROW(boost::url{"192.168.1.1:12345"}, boost::system::system_error);

    boost::url test{"adb://192.168.1.1:12345"};
    EXPECT_EQ(test.host_ipv4_address().to_string(), "192.168.1.1");
    EXPECT_EQ(test.port_number(), 12345);
    EXPECT_EQ(test.authority().buffer(), std::string_view{"192.168.1.1:12345"});
    EXPECT_EQ(test.scheme(), "adb");

    boost::url test2{"hwnd://0x34143643"};
    EXPECT_EQ(test2.host(), "0x34143643");
    EXPECT_EQ(test2.authority().buffer(), std::string_view{"0x34143643"});
    EXPECT_EQ(test2.scheme(), "hwnd");

    boost::url test3{"hwnd://34143643"};
    EXPECT_EQ(test3.host(), "34143643");
    EXPECT_EQ(test3.authority().buffer(), std::string_view{"34143643"});
    EXPECT_EQ(test3.scheme(), "hwnd");
}
