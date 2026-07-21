#include "pch.h"
#include "../CoreEngine/XmlToSsmlMapper.h"

TEST(XmlToSsmlMapperTests, PlainTextIsWrappedInSpeakRoot) {
    std::u16string input = u"Hello World";
    std::u16string expected = u"<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"en-US\">Hello World</speak>";
    
    std::u16string result = XmlToSsmlMapper::Translate(input);
    EXPECT_TRUE(result == expected);
}

TEST(XmlToSsmlMapperTests, PitchTagIsTranslatedToProsody) {
    std::u16string input = u"<PITCH MIDDLE=\"5\">Hello</PITCH>";
    // Our dummy translation maps <PITCH> to <prosody pitch="default"> and </PITCH> to </prosody>
    std::u16string expected = u"<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"en-US\"><prosody pitch=\"default\">Hello</prosody></speak>";
    
    std::u16string result = XmlToSsmlMapper::Translate(input);
    EXPECT_TRUE(result == expected);
}
