# Task 1 Review Package: Characterize Ignored Punctuation Through the Real Control Pipe

Base commit: fa25d83c3830cb30af66904fa3bb8f5d7baf6221
Working tree diff:

```diff
diff --git a/CoreEngine.Tests/ProtocolParsingTests.cpp b/CoreEngine.Tests/ProtocolParsingTests.cpp
index 7483a1b..e342579 100644
--- a/CoreEngine.Tests/ProtocolParsingTests.cpp
+++ b/CoreEngine.Tests/ProtocolParsingTests.cpp
@@ -16,6 +16,9 @@ TEST(SpeechProtocolUtilsTests, ParseProviderEventTypeMapsAllKnownStrings)
     EXPECT_EQ(ParseProviderEventType("synthesis_cancelled"), ProviderEventType::SynthesisCancelled);
     EXPECT_EQ(ParseProviderEventType("log"), ProviderEventType::Log);
     EXPECT_EQ(ParseProviderEventType("completed"), ProviderEventType::LegacyCompleted);
+    EXPECT_EQ(ParseProviderEventType("punctuation_boundary"), ProviderEventType::Unknown);
+    EXPECT_EQ(ParseProviderEventType("viseme"), ProviderEventType::Unknown);
+    EXPECT_EQ(ParseProviderEventType("viseme_reached"), ProviderEventType::Unknown);
     EXPECT_EQ(ParseProviderEventType("unknown_event"), ProviderEventType::Unknown);
     EXPECT_EQ(ParseProviderEventType(""), ProviderEventType::Unknown);
 }
diff --git a/CoreEngine.Tests/WorkerFaultTests.cpp b/CoreEngine.Tests/WorkerFaultTests.cpp
index d003854..120a71e 100644
--- a/CoreEngine.Tests/WorkerFaultTests.cpp
+++ b/CoreEngine.Tests/WorkerFaultTests.cpp
@@ -155,6 +155,47 @@ TEST_F(SapiEngineTests, LegacyCompletedFollowedByRealTerminalCompletesSuccessful
     }
 }

+TEST_F(SapiEngineTests, PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault)
+{
+    PipeServerWorkerFixture fixture;
+    ASSERT_TRUE(fixture.Initialize());
+    ASSERT_TRUE(fixture.Start(75));
+
+#if defined(_DEBUG)
+    ClearTestLogs();
+    fixture.worker->PauseNextEventForwardForTest();
+#endif
+
+    ASSERT_TRUE(fixture.server.WriteControl(
+        "{\"event\":\"punctuation_boundary\",\"speak_id\":75,\"audio_offset_ms\":100,\"text_offset\":5,\"text_length\":1}\n"));
+
+#if defined(_DEBUG)
+    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));
+
+    {
+        const auto logs = GetTestLogs();
+        const bool foundDiagnostic = std::any_of(logs.begin(), logs.end(), [](const std::wstring &line)
+        {
+            return line.find(L"Unknown event received") != std::wstring::npos;
+        });
+        EXPECT_TRUE(foundDiagnostic);
+    }
+
+    fixture.worker->ReleaseEventForwardForTest();
+#endif
+
+    ASSERT_TRUE(fixture.server.WriteControl(
+        "{\"event\":\"synthesis_complete\",\"speak_id\":75,\"total_audio_bytes\":0}\n"));
+
+    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
+    EXPECT_FALSE(fixture.worker->IsFaulted());
+
+    {
+        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
+        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
+    }
+}
+
 #if defined(_DEBUG)
 TEST_F(SapiEngineTests, UnknownNamedEventWithActiveSpeakIdDoesNotFaultWorker)
 {
```
