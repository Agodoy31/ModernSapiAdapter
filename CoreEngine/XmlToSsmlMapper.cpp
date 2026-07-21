#include "pch.h"
#include "XmlToSsmlMapper.h"

static std::wstring ToWString(const std::u16string& str) {
    return std::wstring(str.begin(), str.end());
}
static std::u16string ToU16String(const std::wstring& str) {
    return std::u16string(str.begin(), str.end());
}

const std::map<std::u16string, XmlToSsmlMapper::TagHandler>& XmlToSsmlMapper::GetHandlers()
{
    static std::map<std::u16string, TagHandler> handlers = {
        { u"PITCH", [](const std::u16string& attributes, bool isClosing) {
            if (isClosing) return std::u16string(u"</prosody>");
            return std::u16string(u"<prosody pitch=\"default\">"); 
        }},
        { u"RATE", [](const std::u16string& attributes, bool isClosing) {
            if (isClosing) return std::u16string(u"</prosody>");
            return std::u16string(u"<prosody rate=\"default\">");
        }},
        { u"VOLUME", [](const std::u16string& attributes, bool isClosing) {
            if (isClosing) return std::u16string(u"</prosody>");
            return std::u16string(u"<prosody volume=\"default\">");
        }}
    };
    return handlers;
}

std::u16string XmlToSsmlMapper::Translate(const std::u16string& sapiXml)
{
    std::wstring input = ToWString(sapiXml);
    std::wstring result;
    
    std::wregex tagRegex(LR"(<(/)?([a-zA-Z0-9_]+)([^>]*)>)");
    std::wsregex_iterator it(input.begin(), input.end(), tagRegex);
    std::wsregex_iterator end;

    size_t lastPos = 0;
    
    result += L"<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"en-US\">";

    auto& handlers = GetHandlers();

    for (; it != end; ++it)
    {
        result += input.substr(lastPos, it->position() - lastPos);
        lastPos = it->position() + it->length();

        bool isClosing = it->str(1) == L"/";
        std::u16string tagName = ToU16String(it->str(2));
        
        for (auto& c : tagName) c = (char16_t)std::towupper((wint_t)c);
        
        std::u16string attributes = ToU16String(it->str(3));

        auto handlerIt = handlers.find(tagName);
        if (handlerIt != handlers.end())
        {
            result += ToWString(handlerIt->second(attributes, isClosing));
        }
    }
    result += input.substr(lastPos);
    result += L"</speak>";

    return ToU16String(result);
}
