#include "Settings.hpp"

#include <Windows.h>

#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
    std::string escapeJson(const std::string& input)
    {
        std::string output;
        output.reserve(input.size() + 8);
        for (char ch : input)
        {
            switch (ch)
            {
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output += ch;
                break;
            }
        }
        return output;
    }

    std::string unescapeJson(const std::string& input)
    {
        std::string output;
        output.reserve(input.size());
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            const char ch = input[i];
            if (ch == '\\' && i + 1 < input.size())
            {
                const char next = input[++i];
                switch (next)
                {
                case 'n': output += '\n'; break;
                case 'r': output += '\r'; break;
                case 't': output += '\t'; break;
                case '"': output += '"'; break;
                case '\\': output += '\\'; break;
                default:
                    output += next;
                    break;
                }
            }
            else
            {
                output += ch;
            }
        }
        return output;
    }

    std::string makeQuotedKey(const std::string& key)
    {
        return std::string("\"") + key + "\"";
    }

    std::string trim(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
        {
            return {};
        }
        const auto end = value.find_last_not_of(" \t\n\r");
        return value.substr(start, end - start + 1);
    }

    std::string extractRawValue(const std::string& content, const std::string& key)
    {
        const std::string token = makeQuotedKey(key);
        auto pos = content.find(token);
        if (pos == std::string::npos)
        {
            return {};
        }
        pos = content.find(':', pos + token.size());
        if (pos == std::string::npos)
        {
            return {};
        }
        ++pos; // move past ':'
        std::size_t end = pos;
        bool inString = false;
        bool escaped = false;
        int braceDepth = 0;
        while (end < content.size())
        {
            char ch = content[end];
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (ch == '\\')
                {
                    escaped = true;
                }
                else if (ch == '"')
                {
                    inString = false;
                }
            }
            else
            {
                if (ch == '"')
                {
                    inString = true;
                }
                else if (ch == '{')
                {
                    ++braceDepth;
                }
                else if (ch == '}')
                {
                    if (braceDepth == 0)
                    {
                        break;
                    }
                    --braceDepth;
                }
                else if (ch == ',')
                {
                    if (braceDepth == 0)
                    {
                        break;
                    }
                }
            }
            ++end;
        }
        return trim(content.substr(pos, end - pos));
    }

    bool tryParseBool(const std::string& content, const std::string& key, bool& value)
    {
        const std::string raw = extractRawValue(content, key);
        if (raw.empty())
        {
            return false;
        }
        if (raw == "true")
        {
            value = true;
            return true;
        }
        if (raw == "false")
        {
            value = false;
            return true;
        }
        return false;
    }

    bool tryParseUInt(const std::string& content, const std::string& key, unsigned int& value)
    {
        const std::string raw = extractRawValue(content, key);
        if (raw.empty())
        {
            return false;
        }
        try
        {
            value = static_cast<unsigned int>(std::stoul(raw));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool tryParseString(const std::string& content, const std::string& key, std::string& value)
    {
        std::string raw = extractRawValue(content, key);
        if (raw.empty())
        {
            return false;
        }
        if (raw.size() < 2 || raw.front() != '"')
        {
            return false;
        }
        if (raw.back() != '"')
        {
            // Handle possible trailing object where last character is '}'
            const auto quotePos = raw.find_last_of('"');
            if (quotePos == std::string::npos)
            {
                return false;
            }
            raw = raw.substr(0, quotePos + 1);
        }
        const std::string inner = raw.substr(1, raw.size() - 2);
        value = unescapeJson(inner);
        return true;
    }

    void parseMenuHotkey(const std::string& content, HotkeyConfig& hotkey)
    {
        const std::string key = makeQuotedKey("menuHotkey");
        auto pos = content.find(key);
        if (pos == std::string::npos)
        {
            return;
        }
        pos = content.find('{', pos + key.size());
        if (pos == std::string::npos)
        {
            return;
        }
        int depth = 1;
        std::size_t end = pos + 1;
        while (end < content.size() && depth > 0)
        {
            char ch = content[end];
            if (ch == '{')
            {
                ++depth;
            }
            else if (ch == '}')
            {
                --depth;
                if (depth == 0)
                {
                    ++end;
                    break;
                }
            }
            ++end;
        }
        if (depth != 0)
        {
            return;
        }
        const std::string inner = content.substr(pos + 1, end - pos - 2);

        hotkey.chordVirtualKey = 0;

        auto parseVkToken = [](const std::string& token, unsigned int& out) {
            if (token.empty())
            {
                return false;
            }
            if (token == "VK_INSERT")
            {
                out = VK_INSERT;
                return true;
            }
            if (token == "VK_PRIOR")
            {
                out = VK_PRIOR;
                return true;
            }
            if (token == "VK_NEXT")
            {
                out = VK_NEXT;
                return true;
            }
            if (token == "VK_HOME")
            {
                out = VK_HOME;
                return true;
            }
            if (token == "VK_END")
            {
                out = VK_END;
                return true;
            }
            if (token.rfind("VK_0x", 0) == 0 || token.rfind("VK_0X", 0) == 0)
            {
                try
                {
                    const auto numeric = std::stoul(token.substr(4), nullptr, 16);
                    out = static_cast<unsigned int>(numeric);
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            }
            return false;
        };

        std::string vkName;
        if (tryParseString(inner, "virtualKey", vkName))
        {
            parseVkToken(vkName, hotkey.virtualKey);
        }
        std::string chordName;
        if (tryParseString(inner, "chordVirtualKey", chordName))
        {
            if (!parseVkToken(chordName, hotkey.chordVirtualKey))
            {
                hotkey.chordVirtualKey = 0;
            }
        }
        tryParseBool(inner, "requireCtrl", hotkey.requireCtrl);
        tryParseBool(inner, "requireRightCtrl", hotkey.requireRightCtrl);
        tryParseBool(inner, "requireShift", hotkey.requireShift);
        tryParseBool(inner, "requireAlt", hotkey.requireAlt);
        tryParseBool(inner, "requireWin", hotkey.requireWin);
    }
}

SettingsManager::SettingsManager()
    : settingsFile_(determineSettingsPath())
{
}

HotkeyConfig SettingsManager::defaultMenuHotkey()
{
    HotkeyConfig config;
    config.virtualKey = 'M';
    config.chordVirtualKey = 0;
    config.requireCtrl = true;
    config.requireRightCtrl = false;
    config.requireShift = false;
    config.requireAlt = true;
    config.requireWin = false;
    return config;
}

std::filesystem::path SettingsManager::determineSettingsPath()
{
    wchar_t buffer[MAX_PATH];
    DWORD written = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (written == 0)
    {
        return std::filesystem::current_path() / "settings.json";
    }
    std::filesystem::path path(buffer, buffer + written);
    path = path.parent_path() / "settings.json";
    return path;
}

AppSettings SettingsManager::load()
{
    AppSettings settings;
    settings.menuHotkey = defaultMenuHotkey();

    std::ifstream file(settingsFile_, std::ios::binary);
    if (!file.is_open())
    {
        return settings;
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    const std::string content = oss.str();

    tryParseString(content, "videoDeviceMoniker", settings.videoDeviceMoniker);
    tryParseString(content, "audioDeviceMoniker", settings.audioDeviceMoniker);
    tryParseBool(content, "audioPlaybackEnabled", settings.audioPlaybackEnabled);
    tryParseBool(content, "mouseAbsoluteMode", settings.mouseAbsoluteMode);
    tryParseUInt(content, "videoPreferredWidth", settings.videoPreferredWidth);
    tryParseUInt(content, "videoPreferredHeight", settings.videoPreferredHeight);
    tryParseBool(content, "videoAllowResizing", settings.videoAllowResizing);

    if (settings.videoPreferredWidth == 0 || settings.videoPreferredHeight == 0)
    {
        settings.videoPreferredWidth = 0;
        settings.videoPreferredHeight = 0;
    }

    unsigned int aspectModeValue = static_cast<unsigned int>(settings.videoAspectMode);
    if (tryParseUInt(content, "videoAspectMode", aspectModeValue))
    {
        if (aspectModeValue <= static_cast<unsigned int>(VideoAspectMode::Capture))
        {
            settings.videoAspectMode = static_cast<VideoAspectMode>(aspectModeValue);
        }
    }
    else
    {
        bool legacyForceAspect = true;
        if (tryParseBool(content, "videoForceAspectRatio", legacyForceAspect))
        {
            settings.videoAspectMode = legacyForceAspect ? VideoAspectMode::Maintain : VideoAspectMode::Stretch;
        }
    }
    parseMenuHotkey(content, settings.menuHotkey);

    const bool legacyMenuHotkey =
        settings.menuHotkey.virtualKey == VK_INSERT &&
        settings.menuHotkey.chordVirtualKey == 0 &&
        settings.menuHotkey.requireCtrl &&
        settings.menuHotkey.requireRightCtrl &&
        !settings.menuHotkey.requireShift &&
        !settings.menuHotkey.requireAlt &&
        !settings.menuHotkey.requireWin;

    const bool legacyHomeMenuHotkey =
        settings.menuHotkey.virtualKey == VK_HOME &&
        settings.menuHotkey.chordVirtualKey == VK_PRIOR &&
        !settings.menuHotkey.requireCtrl &&
        !settings.menuHotkey.requireRightCtrl &&
        !settings.menuHotkey.requireShift &&
        !settings.menuHotkey.requireAlt &&
        !settings.menuHotkey.requireWin;

    if (legacyMenuHotkey || legacyHomeMenuHotkey)
    {
        settings.menuHotkey = defaultMenuHotkey();
    }

    return settings;
}

void SettingsManager::save(const AppSettings& settings) const
{
    if (settingsFile_.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(settingsFile_.parent_path(), ec);
    }

    std::ofstream file(settingsFile_, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return;
    }

    file << "{\n";
    file << "  \"videoDeviceMoniker\": \"" << escapeJson(settings.videoDeviceMoniker) << "\",\n";
    file << "  \"audioDeviceMoniker\": \"" << escapeJson(settings.audioDeviceMoniker) << "\",\n";
    file << "  \"audioPlaybackEnabled\": " << (settings.audioPlaybackEnabled ? "true" : "false") << ",\n";
    file << "  \"mouseAbsoluteMode\": " << (settings.mouseAbsoluteMode ? "true" : "false") << ",\n";
    file << "  \"videoPreferredWidth\": " << settings.videoPreferredWidth << ",\n";
    file << "  \"videoPreferredHeight\": " << settings.videoPreferredHeight << ",\n";
    file << "  \"videoAllowResizing\": " << (settings.videoAllowResizing ? "true" : "false") << ",\n";
    file << "  \"videoAspectMode\": " << static_cast<unsigned int>(settings.videoAspectMode) << ",\n";
    file << "  \"menuHotkey\": {\n";
    file << "    \"virtualKey\": \"VK_0x";
    file << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << settings.menuHotkey.virtualKey;
    file << std::nouppercase << std::dec << std::setfill(' ') << "\",\n";
    file << "    \"chordVirtualKey\": \"VK_0x";
    file << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << settings.menuHotkey.chordVirtualKey;
    file << std::nouppercase << std::dec << std::setfill(' ') << "\",\n";
    file << "    \"requireCtrl\": " << (settings.menuHotkey.requireCtrl ? "true" : "false") << ",\n";
    file << "    \"requireRightCtrl\": " << (settings.menuHotkey.requireRightCtrl ? "true" : "false") << ",\n";
    file << "    \"requireShift\": " << (settings.menuHotkey.requireShift ? "true" : "false") << ",\n";
    file << "    \"requireAlt\": " << (settings.menuHotkey.requireAlt ? "true" : "false") << ",\n";
    file << "    \"requireWin\": " << (settings.menuHotkey.requireWin ? "true" : "false") << "\n";
    file << "  }\n";
    file << "}\n";
}
