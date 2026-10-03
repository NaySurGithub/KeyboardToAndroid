@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" || exit /b 1
copy /y "%~dp0build\KeyboardToAndroid.exe" "%~dp0KeyboardToAndroid.exe" >nul
echo Built %~dp0KeyboardToAndroid.exe
