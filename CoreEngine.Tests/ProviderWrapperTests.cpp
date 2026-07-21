#include "pch.h"
#include "../CoreEngine/ProviderWrapper.h"

class ProviderWrapperTests : public ::testing::Test {
protected:
    ProviderWrapper wrapper;
};

TEST_F(ProviderWrapperTests, LoadFailsOnInvalidPath) {
    HRESULT hr = wrapper.Load(L"NonExistentPath_XYZ123.dll");
    EXPECT_TRUE(hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
    EXPECT_FALSE(wrapper.IsLoaded());
}

TEST_F(ProviderWrapperTests, LoadSucceedsOnMockProvider) {
    // Note: This relies on MockProvider.dll being built and in the same directory or PATH.
    // In VS, we usually output to the same bin directory.
    HRESULT hr = wrapper.Load(L"MockProvider.dll");
    EXPECT_TRUE(hr == S_OK);
    EXPECT_TRUE(wrapper.IsLoaded());
}

TEST_F(ProviderWrapperTests, UnloadClearsState) {
    wrapper.Load(L"MockProvider.dll");
    EXPECT_TRUE(wrapper.IsLoaded());

    wrapper.Unload();
    EXPECT_FALSE(wrapper.IsLoaded());
}

TEST_F(ProviderWrapperTests, ProviderSpeakCallsAbi) {
    HRESULT hr = wrapper.Load(L"MockProvider.dll");
    ASSERT_TRUE(hr == S_OK);

    ProviderSpeakParams params = {};
    params.ContractVersion = PROVIDER_ABI_VERSION;
    
    // We expect the MockProvider to return true from our dummy implementation
    bool success = wrapper.Speak(&params);
    EXPECT_TRUE(success);
}
