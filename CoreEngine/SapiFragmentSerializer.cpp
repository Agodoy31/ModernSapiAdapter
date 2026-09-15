/**
 * @file SapiFragmentSerializer.cpp
 * @brief Implementation of stateless SAPI 5 text fragment serializer.
 */

#include "pch.h"
#include "SapiFragmentSerializer.h"
#include "StringUtils.h"

namespace
{
    [[nodiscard]] std::string FragmentUtf8Text(const SPVTEXTFRAG* fragment)
    {
        if (!fragment || !fragment->pTextStart || fragment->ulTextLen == 0)
        {
            return std::string{};
        }

        return StringUtils::WideToUtf8(fragment->pTextStart, fragment->ulTextLen);
    }
}

namespace SapiFragmentSerializer
{
    nlohmann::json Serialize(const SPVTEXTFRAG* fragmentList)
    {
        nlohmann::json fragments = nlohmann::json::array();
        const SPVTEXTFRAG* pFrag = fragmentList;
        while (pFrag)
        {
            nlohmann::json fragJson;

            switch (pFrag->State.eAction)
            {
            case SPVA_Bookmark:
            {
                const std::string bookmarkText = FragmentUtf8Text(pFrag);
                if (!bookmarkText.empty())
                {
                    fragJson["bookmark"] = bookmarkText;
                }
                break;
            }

            case SPVA_Silence:
            {
                fragJson["silence_ms"] = pFrag->State.SilenceMSecs;
                break;
            }

            case SPVA_Speak:
            case SPVA_Pronounce:
            case SPVA_SpellOut:
            case SPVA_Section:
            case SPVA_ParseUnknownTag:
            {
                const std::string speechText = FragmentUtf8Text(pFrag);
                if (!speechText.empty())
                {
                    fragJson["text"] = speechText;
                    fragJson["source_offset"] = pFrag->ulTextSrcOffset;
                }
                fragJson["volume"] = pFrag->State.Volume;
                fragJson["pitch"] = pFrag->State.PitchAdj.MiddleAdj;
                fragJson["rate"] = pFrag->State.RateAdj;
                break;
            }

            default:
            {
                break;
            }
            }

            fragments.push_back(fragJson);
            pFrag = pFrag->pNext;
        }

        return fragments;
    }
}
