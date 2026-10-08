// SPDX-License-Identifier: MIT

// The console the command-line tools print to. Dropping files on csfrip.exe or csfplay.exe, or double-clicking one,
// gives the tool a console window of its own. That window closes as soon as the tool exits.

#pragma once

namespace csf
{

// Sets the Windows console to UTF-8 and restores its code page on exit. Does nothing on other platforms.
void SetUpConsole();

// If the tool has a Windows console window of its own, waits for Enter before the tool exits. The output can then be
// read. Does nothing elsewhere.
void KeepConsoleOpen();

} // namespace csf
