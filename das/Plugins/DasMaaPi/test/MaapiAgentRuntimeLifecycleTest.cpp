#include "../src/MaaHandle.h"
#include "FakeMaaApiBoundary.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    TEST(DasMaaPiMaaHandle, MaaHandlesCleanupInReverseOwnershipOrder)
    {
        FakeMaaApiBoundary fake;

        {
            ScopedResource resource(fake, fake.CreateResource());
            ScopedController controller(
                fake,
                fake.CreateController(
                    ControllerSpec{.name = "Android", .type = "Adb"}));
            ScopedTasker tasker(fake, fake.CreateTasker());
        }

        const std::vector<std::string> expected{
            "CreateResource",
            "CreateController:Android:Adb",
            "CreateTasker",
            "DestroyTasker:3",
            "DestroyController:2",
            "DestroyResource:1"};
        EXPECT_EQ(fake.calls, expected);
    }

    TEST(DasMaaPiMaaHandle, InvalidMaaHandlesDoNotCallDestroy)
    {
        FakeMaaApiBoundary fake;

        {
            ScopedResource resource(fake, kInvalidMaaResourceHandle);
            ScopedController controller(fake, kInvalidMaaControllerHandle);
            ScopedTasker tasker(fake, kInvalidMaaTaskerHandle);
            ScopedAgentClient client(fake, kInvalidMaaAgentClientHandle);
        }

        EXPECT_TRUE(fake.calls.empty());
    }

    TEST(DasMaaPiAgentClient, FakeBoundaryDrivesAgentClientOperations)
    {
        FakeMaaApiBoundary fake;

        const auto client = fake.CreateAgentClientV2(std::string_view("agent-1"));
        EXPECT_NE(client, kInvalidMaaAgentClientHandle);
        EXPECT_EQ(fake.GetAgentClientIdentifier(client), "agent-client-id");
        EXPECT_TRUE(fake.BindAgentClientResource(client, 10).ok);
        EXPECT_TRUE(fake.RegisterAgentClientResourceSink(client, 10).ok);
        EXPECT_TRUE(fake.RegisterAgentClientControllerSink(client, 11).ok);
        EXPECT_TRUE(fake.RegisterAgentClientTaskerSink(client, 12).ok);
        EXPECT_TRUE(fake.SetAgentClientTimeout(client, 5000).ok);
        EXPECT_TRUE(fake.ConnectAgentClient(client).ok);
        EXPECT_TRUE(fake.IsAgentClientConnected(client));
        EXPECT_TRUE(fake.IsAgentClientAlive(client));

        const std::vector<std::string> expected{
            "CreateAgentClientV2:agent-1",
            "GetAgentClientIdentifier",
            "BindAgentClientResource",
            "RegisterAgentClientResourceSink",
            "RegisterAgentClientControllerSink",
            "RegisterAgentClientTaskerSink",
            "SetAgentClientTimeout:5000",
            "ConnectAgentClient",
            "IsAgentClientConnected",
            "IsAgentClientAlive"};
        EXPECT_EQ(fake.calls, expected);
    }

    TEST(DasMaaPiAgentClient, ScopedAgentClientDisconnectsBeforeDestroy)
    {
        FakeMaaApiBoundary fake;

        {
            ScopedAgentClient client(fake, fake.CreateAgentClientTcp(0));
        }

        const std::vector<std::string> expected{
            "CreateAgentClientTcp:0",
            "DisconnectAgentClient:1",
            "DestroyAgentClient:1"};
        EXPECT_EQ(fake.calls, expected);
    }
} // namespace
