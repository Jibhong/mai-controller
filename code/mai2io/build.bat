@echo off
rd /s /q build
cmake -B build -A x64
cmake --build build --config Release
copy .\build\Release\mai2io.dll .\mai2io.dll
