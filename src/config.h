#pragma once

#include "hotkey.h"

struct HostConfig
{
    Hotkey input;
    Hotkey overlay;
    std::wstring targetExecutable;
    bool enableDepth;
};

// Reads shortcuts and settings from RobloxShadeHost.ini beside the exe, creating the file on first run.
// Shows an error and throws when the value cannot be parsed.
HostConfig LoadConfig();
