#pragma once

#include <windows.h>
#include <string>

// Finds the main window of a running target client, or nullptr. The game is only observed from outside,
// through process and window enumeration. Nothing is opened, read or loaded into its process.
HWND FindTargetWindow(const std::wstring& targetExe);
