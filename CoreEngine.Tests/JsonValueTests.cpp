#include "pch.h"
#include "../CoreEngine/JsonValue.h"
#include <limits>
#include <vector>

TEST(JsonValueTests, TryGetJsonUnsignedIntegerAcceptsCompatibleNumberRepresentations)
{
    uint32_t output = 0;

    EXPECT_TRUE(TryGetJsonUnsignedInteger(nlohmann::json(17u), output));
    EXPECT_EQ(output, 17u);

    EXPECT_TRUE(TryGetJsonUnsignedInteger(nlohmann::json(18), output));
    EXPECT_EQ(output, 18u);

    EXPECT_TRUE(TryGetJsonUnsignedInteger(nlohmann::json(19.0), output));
    EXPECT_EQ(output, 19u);
}

TEST(JsonValueTests, TryGetJsonUnsignedIntegerRejectsIncompatibleValuesWithoutChangingOutput)
{
    const std::vector<nlohmann::json> invalidValues{
        nlohmann::json(-1),
        nlohmann::json(1.5),
        nlohmann::json(std::numeric_limits<double>::infinity()),
        nlohmann::json(9007199254740992.0),
        nlohmann::json(4294967296ull),
        nlohmann::json(static_cast<double>((std::numeric_limits<uint64_t>::max)())),
        nlohmann::json(true),
        nlohmann::json("17"),
        nlohmann::json(nullptr),
        nlohmann::json::array(),
        nlohmann::json::object()};

    for (const auto &invalidValue : invalidValues)
    {
        uint32_t output = 123u;
        EXPECT_FALSE(TryGetJsonUnsignedInteger(invalidValue, output));
        EXPECT_EQ(output, 123u);
    }
}
