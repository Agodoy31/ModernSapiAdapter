#include "pch.h"
#include "../CoreEngine/SpeechWorker.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/ProviderWrapper.h"

// Instantiate a real CSapiEngine as a lightweight context container.

class SpeechWorkerTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize COM for the test thread just in case CSapiEngine needs it
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    void TearDown() override {
        CoUninitialize();
    }
};

TEST_F(SpeechWorkerTests, StartAndWaitUntilFinished) {
    auto engine = winrt::make_self<CSapiEngine>();

    ProviderWrapper wrapper;
    SpeechWorker worker(engine.get(), &wrapper);

    // It should not be running initially
    EXPECT_TRUE(worker.IsFinished());

    // Because there's no provider loaded, if we start, it should just exit immediately
    worker.Start(u"Hello world", 0.0f, 0.0f, 100.0f);
    worker.WaitUntilFinished();
    EXPECT_TRUE(worker.IsFinished());
}

TEST_F(SpeechWorkerTests, StopSignalsAbort) {
    ProviderWrapper wrapper;
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &wrapper);

    worker.Start(u"Test", 0.0f, 0.0f, 100.0f);
    worker.Stop();
    
    // Stop() sets the abort flag. 
    worker.WaitUntilFinished();
    EXPECT_TRUE(worker.IsFinished());
}
