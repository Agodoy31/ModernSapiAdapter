#include "pch.h"
#include "ControlStreamBuffer.h"

TEST(ControlStreamBufferTests, TryExtractLine_SingleLineWithLf)
{
    ControlStreamBuffer buffer;
    const std::string data = "{\"command\":\"ready\"}\n";
    buffer.Append(data.data(), data.size());

    std::string_view line;
    EXPECT_TRUE(buffer.TryExtractLine(line));
    EXPECT_EQ(line, "{\"command\":\"ready\"}");
    EXPECT_FALSE(buffer.TryExtractLine(line));
    EXPECT_EQ(buffer.UnconsumedBytes(), 0);
}

TEST(ControlStreamBufferTests, TryExtractLine_SingleLineWithCrLf)
{
    ControlStreamBuffer buffer;
    const std::string data = "{\"command\":\"ready\"}\r\n";
    buffer.Append(data.data(), data.size());

    std::string_view line;
    EXPECT_TRUE(buffer.TryExtractLine(line));
    EXPECT_EQ(line, "{\"command\":\"ready\"}");
    EXPECT_FALSE(buffer.TryExtractLine(line));
    EXPECT_EQ(buffer.UnconsumedBytes(), 0);
}

TEST(ControlStreamBufferTests, TryExtractLine_MultipleLinesInOneChunk)
{
    ControlStreamBuffer buffer;
    const std::string data = "{\"msg\":1}\n{\"msg\":2}\r\n{\"msg\":3}\n";
    buffer.Append(data.data(), data.size());

    std::string_view line1;
    EXPECT_TRUE(buffer.TryExtractLine(line1));
    EXPECT_EQ(line1, "{\"msg\":1}");

    std::string_view line2;
    EXPECT_TRUE(buffer.TryExtractLine(line2));
    EXPECT_EQ(line2, "{\"msg\":2}");

    std::string_view line3;
    EXPECT_TRUE(buffer.TryExtractLine(line3));
    EXPECT_EQ(line3, "{\"msg\":3}");

    std::string_view line4;
    EXPECT_FALSE(buffer.TryExtractLine(line4));
    EXPECT_EQ(buffer.UnconsumedBytes(), 0);
}

TEST(ControlStreamBufferTests, TryExtractLine_FragmentedAppends)
{
    ControlStreamBuffer buffer;
    const std::string part1 = "{\"command\":\"sp";
    const std::string part2 = "eak\",\"text\":\"hel";
    const std::string part3 = "lo\"}\n";

    buffer.Append(part1.data(), part1.size());
    std::string_view line;
    EXPECT_FALSE(buffer.TryExtractLine(line));

    buffer.Append(part2.data(), part2.size());
    EXPECT_FALSE(buffer.TryExtractLine(line));

    buffer.Append(part3.data(), part3.size());
    EXPECT_TRUE(buffer.TryExtractLine(line));
    EXPECT_EQ(line, "{\"command\":\"speak\",\"text\":\"hello\"}");
    EXPECT_FALSE(buffer.TryExtractLine(line));
}

TEST(ControlStreamBufferTests, Compact_FullyConsumedClearsBuffer)
{
    ControlStreamBuffer buffer;
    const std::string data = "{\"msg\":1}\n";
    buffer.Append(data.data(), data.size());

    std::string_view line;
    ASSERT_TRUE(buffer.TryExtractLine(line));
    EXPECT_EQ(buffer.UnconsumedBytes(), 0);

    buffer.Compact();
    EXPECT_EQ(buffer.buffer.size(), 0);
    EXPECT_EQ(buffer.readOffset, 0);
    EXPECT_EQ(buffer.searchOffset, 0);
}

TEST(ControlStreamBufferTests, Compact_PartialReadPreservesUnconsumedData)
{
    ControlStreamBuffer buffer;
    const std::string data = "{\"first\":1}\n{\"second\":2}\n";
    buffer.Append(data.data(), data.size());

    std::string_view line1;
    ASSERT_TRUE(buffer.TryExtractLine(line1));
    EXPECT_EQ(line1, "{\"first\":1}");

    buffer.Compact();
    EXPECT_EQ(buffer.readOffset, 0);
    EXPECT_EQ(buffer.searchOffset, 0);
    EXPECT_EQ(buffer.buffer, "{\"second\":2}\n");

    std::string_view line2;
    EXPECT_TRUE(buffer.TryExtractLine(line2));
    EXPECT_EQ(line2, "{\"second\":2}");
    EXPECT_FALSE(buffer.TryExtractLine(line2));
}

TEST(ControlStreamBufferTests, IsOverCapacity_DetectsOversizedRecords)
{
    ControlStreamBuffer buffer;
    const std::string data(100, 'a');
    buffer.Append(data.data(), data.size());

    EXPECT_FALSE(buffer.IsOverCapacity(150));
    EXPECT_TRUE(buffer.IsOverCapacity(50));
}

TEST(ControlStreamBufferTests, Clear_ResetsAllOffsets)
{
    ControlStreamBuffer buffer;
    const std::string data = "{\"incomplete\":true}";
    buffer.Append(data.data(), data.size());

    std::string_view line;
    EXPECT_FALSE(buffer.TryExtractLine(line));
    EXPECT_GT(buffer.searchOffset, 0);

    buffer.Clear();
    EXPECT_EQ(buffer.buffer.size(), 0);
    EXPECT_EQ(buffer.readOffset, 0);
    EXPECT_EQ(buffer.searchOffset, 0);
    EXPECT_EQ(buffer.UnconsumedBytes(), 0);
}

TEST(ControlStreamBufferTests, HasPendingLine_DetectsExistingNewline)
{
    ControlStreamBuffer buffer;
    EXPECT_FALSE(buffer.HasPendingLine());

    const std::string partial = "{\"part\":1}";
    buffer.Append(partial.data(), partial.size());
    EXPECT_FALSE(buffer.HasPendingLine());

    const std::string newline = "\n";
    buffer.Append(newline.data(), newline.size());
    EXPECT_TRUE(buffer.HasPendingLine());
}
