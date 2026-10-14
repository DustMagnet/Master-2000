# Windows build

The existing project configuration targets .NET 10, Windows SDK 10.0.26100.0,
WinUI 3 and the MSVC v145 toolset. Use a Visual Studio installation with those
components, Desktop development with C++, and WinUI/.NET development installed.
NuGet restores Microsoft.WindowsAppSDK 2.3.1 and NAudio 2.2.1 as pinned in the
project. Windows 10 version 1809 is the declared minimum; actual beta acceptance
must be performed on the Windows systems that will be supported.

Open Master-2000-App/Master-2000.sln and build Release | x64. Set Vinyl2000.App
as startup project. The engine is built and copied beside the GUI.

For the existing single-file publisher run Master-2000-App/BUILD-STANDALONE.cmd.
This script checks that its output contains Master-2000.exe. It does not by itself
prove clean-machine launch or successful native engine extraction. Test those
before distributing that executable.

For a folder-based portable candidate, use BUILD-PORTABLE.cmd from a Visual Studio
Developer Command Prompt. Distribute the entire generated folder, not just the GUI
EXE. This includes license and third-party notice files. Installed runtime files
and their notices must remain present. No signed installer is supplied.

Do not commit bin, obj, .vs, restored files, raw recordings or local diagnostic
runs. Test the release with three and six sources, saved-result loading, playback,
manual start verification, user undo/export and manual donor review.
