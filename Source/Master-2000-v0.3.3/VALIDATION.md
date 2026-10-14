# Validation
XML/XAML parsed, event handlers checked, Sources description row inspected.
Before compiler cleanup, the engine matched v0.3.3b apart from its label.
After cleanup, the targeted edge regression executable passed on GCC C++17.
Windows build 0.3.3.4 supplied; final interface and standalone operation without supporting files were reported tested successfully by the developer.

Compiler cleanup: edge delay is range-checked before explicit conversion to
Windows long; NaN constant marked constexpr. Normal finite edge timing is
unchanged; invalid/out-of-range basins are rejected. Windows rebuild supplied.
