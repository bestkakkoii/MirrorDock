// SPDX-License-Identifier: MIT
// Tests for the interface language: Windows display language names mapped to
// Traditional Chinese / Simplified Chinese / English, and the text lookup of
// the selected language (the table itself is checked at compile time).
import std;
import uxplay_status;
import uxplay_ui_text;

namespace
{
    int failures = 0;

    void check(bool condition, std::string_view what)
    {
        std::println("{} {}", condition ? "ok:  " : "FAIL:", what);
        if (!condition)
        {
            ++failures;
        }
    }
}

int main()
{
    using uxplay::ui_language;
    using uxplay::ui_language_for;
    check(ui_language_for(L"zh-TW") == ui_language::traditional_chinese, "zh-TW is Traditional Chinese");
    check(ui_language_for(L"zh-HK") == ui_language::traditional_chinese, "zh-HK is Traditional Chinese");
    check(ui_language_for(L"zh-MO") == ui_language::traditional_chinese, "zh-MO is Traditional Chinese");
    check(ui_language_for(L"zh-Hant") == ui_language::traditional_chinese, "zh-Hant is Traditional Chinese");
    check(ui_language_for(L"zh-Hant-HK") == ui_language::traditional_chinese, "zh-Hant-HK is Traditional Chinese");
    check(ui_language_for(L"zh_TW") == ui_language::traditional_chinese, "an underscore separator is accepted");
    check(ui_language_for(L"ZH-tw") == ui_language::traditional_chinese, "subtags are case-insensitive");
    check(ui_language_for(L"zh-CN") == ui_language::simplified_chinese, "zh-CN is Simplified Chinese");
    check(ui_language_for(L"zh-SG") == ui_language::simplified_chinese, "zh-SG is Simplified Chinese");
    check(ui_language_for(L"zh-Hans") == ui_language::simplified_chinese, "zh-Hans is Simplified Chinese");
    check(ui_language_for(L"zh-Hans-HK") == ui_language::simplified_chinese, "the script subtag wins over the region");
    check(ui_language_for(L"zh") == ui_language::simplified_chinese, "plain zh is Simplified Chinese");
    check(ui_language_for(L"en-US") == ui_language::english, "en-US is English");
    check(ui_language_for(L"ja-JP") == ui_language::english, "any other language falls back to English");
    check(ui_language_for(L"zhx-TW") == ui_language::english, "only the exact zh language subtag counts");
    check(ui_language_for(L"") == ui_language::english, "an empty name is English");

    uxplay::set_ui_language(ui_language::traditional_chinese);
    check(uxplay::ui_string(uxplay::ui_text::exit) == L"結束" && uxplay::ui_locale_name() == L"zh-TW",
          "Traditional Chinese text and DirectWrite locale");
    uxplay::set_ui_language(ui_language::simplified_chinese);
    check(uxplay::ui_string(uxplay::ui_text::exit) == L"退出" && uxplay::ui_locale_name() == L"zh-CN",
          "Simplified Chinese text and DirectWrite locale");
    uxplay::set_ui_language(ui_language::english);
    check(uxplay::ui_string(uxplay::ui_text::exit) == L"Exit" && uxplay::ui_locale_name() == L"en-US",
          "English text and DirectWrite locale");

    auto const detected = uxplay::detect_ui_language();
    check(detected.has_value() && !detected->windows_language.empty(), "the Windows display language is readable");
    if (detected)
    {
        std::println("  this PC: {} -> {}", uxplay::utf16_to_utf8(detected->windows_language),
                     uxplay::ui_language_name(detected->language));
    }

    std::println("{} failure(s)", failures);
    return failures == 0 ? 0 : 1;
}
