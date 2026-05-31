#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <gtest/gtest.h>

#include <type_traits>

using DAS::Core::IPC::AnyTransport;
using DAS::Core::IPC::IpcResponseSender;
using DAS::Core::IPC::IpcRunLoop;

TEST(IpcResponseSenderTest, SessionRouteConstructorRemainsAvailable)
{
    EXPECT_TRUE((std::is_constructible_v<IpcResponseSender, IpcRunLoop&>));
}

TEST(IpcResponseSenderTest, HostConnectionRouteConstructorRemainsAvailable)
{
    EXPECT_TRUE((std::is_constructible_v<
                 IpcResponseSender,
                 IpcResponseSender::HostConnectionRoute>));
}

TEST(IpcResponseSenderTest, HostConnectionRouteDoesNotUseBorrowedTransport)
{
    EXPECT_FALSE((std::is_constructible_v<IpcResponseSender, AnyTransport&>))
        << "IpcResponseSender must store a response route capability, not an "
           "AnyTransportRef";
    EXPECT_FALSE((
        std::is_constructible_v<IpcResponseSender, AnyTransport&, IpcRunLoop&>))
        << "MainProcess response routes must be owner-bound through a host "
           "connection capability";
}
