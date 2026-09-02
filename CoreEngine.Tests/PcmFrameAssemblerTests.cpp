#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

TEST(PcmFrameAssemblerTests, OneByteFragmentsDoNotProducePartial16BitMonoFrames)
{
    PcmFrameAssembler assembler(2);
    std::vector<size_t> emittedSizes;

    const auto output = FeedPcmFragments(assembler,
        { { 0x10 }, { 0x11 }, { 0x20 }, { 0x21 } }, emittedSizes);

    EXPECT_EQ(emittedSizes, (std::vector<size_t>{ 2, 2 }));
    EXPECT_EQ(output, (std::vector<uint8_t>{ 0x10, 0x11, 0x20, 0x21 }));
    EXPECT_FALSE(assembler.HasCarry());
}

TEST(PcmFrameAssemblerTests, Awkward24BitStereoBoundariesPreserveEveryProviderByte)
{
    PcmFrameAssembler assembler(6);
    std::vector<size_t> emittedSizes;

    const auto output = FeedPcmFragments(assembler,
        { { 0x01 }, { 0x02, 0x03, 0x04, 0x05 }, { 0x06, 0x11, 0x12 },
          { 0x13, 0x14, 0x15, 0x16, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 } }, emittedSizes);

    EXPECT_EQ(emittedSizes, (std::vector<size_t>{ 6, 6, 6 }));
    EXPECT_EQ(output, (std::vector<uint8_t>{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26 }));
    EXPECT_FALSE(assembler.HasCarry());
}

TEST(PcmFrameAssemblerTests, ResetPreventsPartialFrameBytesCrossingRequestBoundaries)
{
    PcmFrameAssembler assembler(6);
    EXPECT_TRUE(assembler.Process(std::vector<uint8_t>{ 0xA1, 0xA2 }.data(), 2).empty());

    assembler.Reset();
    std::vector<size_t> emittedSizes;
    const auto output = FeedPcmFragments(assembler,
        { { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6 } }, emittedSizes);

    EXPECT_EQ(emittedSizes, (std::vector<size_t>{ 6 }));
    EXPECT_EQ(output, (std::vector<uint8_t>{ 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6 }));
    EXPECT_FALSE(assembler.HasCarry());
}

TEST(PcmFrameAssemblerTests, AlignedInputWithoutCarryUsesTheOriginalBuffer)
{
    PcmFrameAssembler assembler(6);
    const std::vector<uint8_t> input{ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                      0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };

    const auto spans = assembler.Process(input.data(), input.size());

    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].data, input.data());
    EXPECT_EQ(spans[0].size, 12u);
}
