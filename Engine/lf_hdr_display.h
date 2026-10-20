// lf_hdr_display.h - is a screen in HDR, and how bright is its SDR white.
// Both come from the display configuration, where the screen is found by its
// GDI device name.
#pragma once
#include <windows.h>
#include <cstring>
#include <vector>

struct HdrDisplayState
{
    bool hdr_on = false;       // advanced colour on, and not forced wide-gamut SDR
    float sdr_white = 1.0f;    // SDR white in units of 80 nits (1.0 when unknown)
};

// The HDR switch in the app: keep HDR through to the output. It is read when
// a source opens; the loop opens the source again when it changes.
static bool HdrSwitchOn()
{
    return smOpt.hdr.load() != 0;
}

// The active display paths, asked until the path list holds still (a mode
// change between the two calls makes the buffers too small).
static bool ActiveDisplayPaths(std::vector<DISPLAYCONFIG_PATH_INFO> &paths)
{
    for (int tries = 0; tries < 3; ++tries)
    {
        UINT32 path_count = 0, mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
            return false;
        paths.resize(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        const LONG answer = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                                               modes.data(), nullptr);
        if (answer == ERROR_INSUFFICIENT_BUFFER) continue;
        if (answer != ERROR_SUCCESS) return false;
        paths.resize(path_count);
        return true;
    }
    return false;
}

static HdrDisplayState HdrDisplayOf(HMONITOR monitor)
{
    HdrDisplayState state;
    MONITORINFOEXW screen = {};
    screen.cbSize = sizeof(screen);
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    if (!GetMonitorInfoW(monitor, &screen) || !ActiveDisplayPaths(paths)) return state;
    for (const DISPLAYCONFIG_PATH_INFO &path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId,
                         path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(source.viewGdiDeviceName, screen.szDevice) != 0)
            continue;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colour = {};
        colour.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(colour), path.targetInfo.adapterId,
                         path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&colour.header) == ERROR_SUCCESS)
            state.hdr_on = colour.advancedColorEnabled && !colour.wideColorEnforced;
        DISPLAYCONFIG_SDR_WHITE_LEVEL level = {};
        level.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(level), path.targetInfo.adapterId,
                        path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS && level.SDRWhiteLevel > 0)
            state.sdr_white = level.SDRWhiteLevel / 1000.0f;
        break;                  // the first path that shows this screen
    }
    return state;
}
