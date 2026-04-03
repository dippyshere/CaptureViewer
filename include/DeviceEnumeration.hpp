#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct VideoDeviceInfo {
    std::string monikerDisplayName;
    std::string friendlyName;
};

struct AudioCaptureDeviceInfo {
    std::string monikerDisplayName;
    std::string friendlyName;
};

struct VideoModeInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frameRate = 0.0;
};

std::vector<VideoDeviceInfo> enumerateVideoCaptureDevices();
std::vector<AudioCaptureDeviceInfo> enumerateAudioCaptureDevices();
std::vector<VideoModeInfo> enumerateVideoModes(const std::string& monikerDisplayName);
