#pragma once

#include <string>
#include <filesystem>

enum class VideoAspectMode : unsigned int {
    Stretch = 0,
    Maintain = 1,
    Capture = 2,
};

enum class VideoFormatPreference : unsigned int {
    Auto = 0,
    XRGB = 1,
    NV12 = 2,
};

struct AppSettings {
    std::string videoDeviceMoniker;
    std::string audioDeviceMoniker;
    bool audioPlaybackEnabled = true;
    bool mouseAbsoluteMode = true;
    unsigned int videoPreferredWidth = 0;
    unsigned int videoPreferredHeight = 0;
    bool videoAllowResizing = true;
    bool videoBorderlessWindowed = true;
    bool videoFullscreen = false;
    bool vsyncEnabled = false;
    VideoAspectMode videoAspectMode = VideoAspectMode::Maintain;
    VideoFormatPreference videoFormatPreference = VideoFormatPreference::Auto;
};

class SettingsManager {
public:
    SettingsManager();

    AppSettings load();
    void save(const AppSettings& settings) const;

    [[nodiscard]] const std::filesystem::path& settingsFile() const noexcept { return settingsFile_; }


private:
    std::filesystem::path settingsFile_;
    static std::filesystem::path determineSettingsPath();
};
