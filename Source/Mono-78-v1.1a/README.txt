Mono-78 v1.1a — English interface

Build on Windows with BUILD.cmd or open Mono-78.sln, Release/x64.
Executable: bin/Release/Mono-78.exe. No prebuilt EXE is included.

Keep playback speed unchanged is selected by default. RPM fields are then
ignored. Mono conversion can still be used.

Presets: 77.92 RPM (Europe), 78.26 RPM (USA), 78.00 RPM, Custom RPM.
Enter the recorded RPM separately, for example 45 to 77.92.
Custom RPM enables the target field. Decimal comma and point are accepted.
Speed factor = target / recorded. Pitch and duration change together.
A record's origin alone does not establish its exact playback speed.

The supplied WAV must be ordinary integer PCM, 8/16/24/32 bit, mono/stereo.
Output is 24-bit PCM at the same sample rate. The input is protected.
AudioCore.h is unchanged; this version translates the interface and guards
UNICODE/_UNICODE definitions to avoid project macro redefinition warnings.

Test the unchanged setting, each preset, custom input and a conversion.
Return the tested Mono-78.exe as a ZIP for the combined release package.
