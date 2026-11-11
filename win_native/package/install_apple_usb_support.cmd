@echo off
rem SPDX-License-Identifier: MIT
rem Installs Apple's USB driver so the optional "AirPlay over the USB cable"
rem path can work (see the user guide, section 3). Not needed for Wi-Fi.
rem Safe to run more than once.
chcp 65001 >nul
echo.
echo Installing Apple Mobile Device Support (Apple USB driver) via winget...
echo A UAC / driver-install prompt may appear - please allow it.
echo.
winget install --id Apple.AppleMobileDeviceSupport --silent --accept-package-agreements --accept-source-agreements
echo.
if %ERRORLEVEL%==0 (
  echo Done. Next: on the iPhone enable Settings ^> Personal Hotspot ^> Allow Others to Join,
  echo keep the USB cable connected, then pick MirrorDock in Screen Mirroring.
) else (
  echo winget returned %ERRORLEVEL%. If winget is missing, install "Apple Devices"
  echo from the Microsoft Store instead, then retry.
)
echo.
pause
