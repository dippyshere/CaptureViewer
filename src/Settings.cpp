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
        int bracketDepth = 0;
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
                    if (braceDepth == 0 && bracketDepth == 0)
                    {
                        break;
                    }
                    if (braceDepth > 0)
                    {
                        --braceDepth;
                    }
                }
                else if (ch == '[')
                {
                    ++bracketDepth;
                }
                else if (ch == ']')
                {
                    if (bracketDepth > 0)
                    {
                        --bracketDepth;
                    }
                }
                else if (ch == ',')
                {
                    if (braceDepth == 0 && bracketDepth == 0)
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

    bool tryParseInt(const std::string& content, const std::string& key, int& value)
    {
        const std::string raw = extractRawValue(content, key);
        if (raw.empty())
        {
            return false;
        }
        try
        {
            value = std::stoi(raw);
            return true;
        }
        catch (...)
        {
            return false;
        }
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

    bool tryParseStringArray(const std::string& content, const std::string& key, std::vector<std::string>& values)
    {
        std::string raw = extractRawValue(content, key);
        if (raw.empty() || raw.front() != '[' || raw.back() != ']')
        {
            return false;
        }

        values.clear();
        std::size_t i = 1;
        while (i + 1 < raw.size())
        {
            while (i + 1 < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n' || raw[i] == '\r' || raw[i] == ','))
            {
                ++i;
            }
            if (i + 1 >= raw.size() || raw[i] == ']')
            {
                break;
            }
            if (raw[i] != '"')
            {
                return false;
            }
            ++i;

            std::string item;
            bool escaped = false;
            while (i < raw.size())
            {
                const char ch = raw[i++];
                if (escaped)
                {
                    switch (ch)
                    {
                    case 'n': item.push_back('\n'); break;
                    case 'r': item.push_back('\r'); break;
                    case 't': item.push_back('\t'); break;
                    case '"': item.push_back('"'); break;
                    case '\\': item.push_back('\\'); break;
                    default: item.push_back(ch); break;
                    }
                    escaped = false;
                    continue;
                }
                if (ch == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (ch == '"')
                {
                    break;
                }
                item.push_back(ch);
            }

            values.push_back(std::move(item));
        }

        return true;
    }
}

SettingsManager::SettingsManager()
    : settingsFile_(determineSettingsPath())
{
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
    tryParseUInt(content, "videoPreferredWidth", settings.videoPreferredWidth);
    tryParseUInt(content, "videoPreferredHeight", settings.videoPreferredHeight);
    tryParseUInt(content, "videoPreferredFrameRate100", settings.videoPreferredFrameRate100);
    tryParseBool(content, "videoAllowResizing", settings.videoAllowResizing);
    tryParseBool(content, "videoBorderlessWindowed", settings.videoBorderlessWindowed);
    tryParseBool(content, "videoFullscreen", settings.videoFullscreen);
    tryParseBool(content, "vsyncEnabled", settings.vsyncEnabled);
    tryParseInt(content, "windowPosX", settings.windowPosX);
    tryParseInt(content, "windowPosY", settings.windowPosY);
    tryParseUInt(content, "windowClientWidth", settings.windowClientWidth);
    tryParseUInt(content, "windowClientHeight", settings.windowClientHeight);
    tryParseBool(content, "hasWindowPlacement", settings.hasWindowPlacement);
    tryParseBool(content, "audioOutputUseDefaultOnly", settings.audioOutputUseDefaultOnly);
    tryParseStringArray(content, "audioOutputDeviceMonikers", settings.audioOutputDeviceMonikers);

    if (settings.videoPreferredWidth == 0 || settings.videoPreferredHeight == 0)
    {
        settings.videoPreferredWidth = 1920;
        settings.videoPreferredHeight = 1080;
    }
    if (settings.videoPreferredFrameRate100 == 0)
    {
        settings.videoPreferredFrameRate100 = 6000;
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

    unsigned int formatPreferenceValue = static_cast<unsigned int>(settings.videoFormatPreference);
    if (tryParseUInt(content, "videoFormatPreference", formatPreferenceValue))
    {
        if (formatPreferenceValue <= static_cast<unsigned int>(VideoFormatPreference::NV12))
        {
            settings.videoFormatPreference = static_cast<VideoFormatPreference>(formatPreferenceValue);
        }
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
    file << "  \"videoPreferredWidth\": " << settings.videoPreferredWidth << ",\n";
    file << "  \"videoPreferredHeight\": " << settings.videoPreferredHeight << ",\n";
    file << "  \"videoPreferredFrameRate100\": " << settings.videoPreferredFrameRate100 << ",\n";
    file << "  \"videoAllowResizing\": " << (settings.videoAllowResizing ? "true" : "false") << ",\n";
    file << "  \"videoBorderlessWindowed\": " << (settings.videoBorderlessWindowed ? "true" : "false") << ",\n";
    file << "  \"videoFullscreen\": " << (settings.videoFullscreen ? "true" : "false") << ",\n";
    file << "  \"vsyncEnabled\": " << (settings.vsyncEnabled ? "true" : "false") << ",\n";
    file << "  \"videoAspectMode\": " << static_cast<unsigned int>(settings.videoAspectMode) << ",\n";
    file << "  \"videoFormatPreference\": " << static_cast<unsigned int>(settings.videoFormatPreference) << ",\n";
    file << "  \"windowPosX\": " << settings.windowPosX << ",\n";
    file << "  \"windowPosY\": " << settings.windowPosY << ",\n";
    file << "  \"windowClientWidth\": " << settings.windowClientWidth << ",\n";
    file << "  \"windowClientHeight\": " << settings.windowClientHeight << ",\n";
    file << "  \"hasWindowPlacement\": " << (settings.hasWindowPlacement ? "true" : "false") << ",\n";
    file << "  \"audioOutputUseDefaultOnly\": " << (settings.audioOutputUseDefaultOnly ? "true" : "false") << ",\n";
    file << "  \"audioOutputDeviceMonikers\": [";
    for (std::size_t i = 0; i < settings.audioOutputDeviceMonikers.size(); ++i)
    {
        if (i != 0)
        {
            file << ", ";
        }
        file << "\"" << escapeJson(settings.audioOutputDeviceMonikers[i]) << "\"";
    }
    file << "]\n";
    file << "}\n";
}
