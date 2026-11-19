#pragma once

#include <string>

namespace app
{
    enum class Language
    {
        Chinese,
        English,
    };

    inline Language& CurrentLanguageRef()
    {
        static Language language = Language::Chinese;
        return language;
    }

    inline Language CurrentLanguage()
    {
        return CurrentLanguageRef();
    }

    inline void SetLanguage(Language language)
    {
        CurrentLanguageRef() = language;
    }

    inline bool IsEnglish()
    {
        return CurrentLanguageRef() == Language::English;
    }

    // 根据当前语言从中文 / 英文里取一个字符串（所有界面文案都通过它切换）。
    inline std::wstring L(wchar_t const* chinese, wchar_t const* english)
    {
        return IsEnglish() ? std::wstring(english) : std::wstring(chinese);
    }
}
