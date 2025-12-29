@echo off
set "JAVA_HOME=C:\Program Files\Android\Android Studio\jbr"
cd /d "c:\Users\jaredgrimes\code\360mu\android"

echo Cleaning build...
call gradlew.bat clean

echo Deleting CMake cache...
rd /s /q ".cxx" 2>nul
rd /s /q "app\.cxx" 2>nul
rd /s /q "app\build\intermediates\cxx" 2>nul

echo Building APK...
call gradlew.bat assembleDebug

if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo Build succeeded!
