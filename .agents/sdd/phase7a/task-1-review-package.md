# Task 1 Review Package

## Diffstat
```text
 CoreEngine.Tests/RequestContextTests.cpp | 30 ++++++++++++++++++++++++++++++
 1 file changed, 30 insertions(+)
```

## Full Diff
```diff
diff --git a/CoreEngine.Tests/RequestContextTests.cpp b/CoreEngine.Tests/RequestContextTests.cpp
index 1e84a76..ed5e891 100644
--- a/CoreEngine.Tests/RequestContextTests.cpp
+++ b/CoreEngine.Tests/RequestContextTests.cpp
@@ -67,3 +67,33 @@ TEST(SpeechProtocolUtilsTests, RequestContext_SemanticPredicatesReflectDiscreteS
     EXPECT_FALSE(ctx.IsAwaitingTerminalAudio());
     EXPECT_TRUE(ctx.IsDrainingCancellation());
 }
+
+TEST(SpeechProtocolUtilsTests, RequestContext_TransitionToCancellingPreservesCompletedUpstreamEnum)
+{
+    RequestContext ctx{};
+    ctx.token.speakId = 42;
+    ctx.token.generation = 7;
+    ctx.upstreamState = UpstreamState::Completed;
+    ctx.downstreamState = DownstreamState::Speaking;
+    ctx.rawAudioBytesRead = 1000;
+    ctx.deliveredAudioBytes = 500;
+    ctx.upstreamTerminalBytes = 2000;
+    ctx.upstreamFinished = true;
+    ctx.faultPending = false;
+    ctx.cancellationDeadlineTick = 0;
+    ctx.completionHr = S_OK;
+
+    ctx.TransitionToCancelling(5000ULL);
+
+    EXPECT_EQ(ctx.upstreamState, UpstreamState::Completed);
+    EXPECT_EQ(ctx.downstreamState, DownstreamState::Cancelling);
+    EXPECT_FALSE(ctx.upstreamFinished);
+    EXPECT_EQ(ctx.upstreamTerminalBytes, 0ULL);
+    EXPECT_EQ(ctx.cancellationDeadlineTick, 5000ULL);
+    EXPECT_EQ(ctx.token.speakId, 42ULL);
+    EXPECT_EQ(ctx.token.generation, 7ULL);
+    EXPECT_EQ(ctx.rawAudioBytesRead, 1000ULL);
+    EXPECT_EQ(ctx.deliveredAudioBytes, 500ULL);
+    EXPECT_EQ(ctx.completionHr, S_OK);
+    EXPECT_TRUE(ctx.IsDrainingCancellation());
+}
```
