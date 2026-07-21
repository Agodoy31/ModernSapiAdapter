/**
 * @file XmlToSsmlMapper.h
 * @brief Header for legacy SAPI 5 XML to standard SSML translator.
 */

#pragma once

/**
 * @class XmlToSsmlMapper
 * @brief Translates legacy SAPI 5 XML strings into standard SSML.
 */
class XmlToSsmlMapper
{
public:
    static std::u16string Translate(const std::u16string& sapiXml);

private:
    using TagHandler = std::function<std::u16string(const std::u16string&, bool)>;
    
    // Maps SAPI 5 XML tag names to their SSML translation handlers
    static const std::map<std::u16string, TagHandler>& GetHandlers();
};
