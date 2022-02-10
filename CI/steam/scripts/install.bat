@echo off
@cd /d "%~dp0"
goto checkAdmin

:checkAdmin
	net session >nul 2>&1
	if %errorLevel% == 0 (
		echo.
	) else (
		echo Administrative rights are required, please re-run this script as Administrator.
		goto end
	)

:writeRegistry
    call :ResolvePath "%~dp0..\"
    SET installdir=%RETVAL%
    reg add "HKLM\SOFTWARE\WOW6432Node\OBS Studio" /f /t REG_SZ /d "%installdir%"
    reg add "HKLM\SOFTWARE\OBS Studio" /f /t REG_SZ /d "%installdir%"

:checkDLL
	echo Checking for 32-bit Virtual Cam registration...
	reg query "HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}" >nul 2>&1
	if %errorLevel% == 0 (
		echo 32-bit Virtual Cam found, skipping install...
		echo.
	) else (
		echo 32-bit Virtual Cam not found, installing...
		goto install32DLL
	)

:CheckDLLContinue
	echo Checking for 64-bit Virtual Cam registration...
	reg query "HKLM\SOFTWARE\Classes\CLSID\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}" >nul 2>&1
	if %errorLevel% == 0 (
		echo 64-bit Virtual Cam found, skipping install...
		echo.
	) else (
		echo 64-bit Virtual Cam not found, installing...
		goto install64DLL
	)
	goto endSuccess

:install32DLL
	echo Installing 32-bit Virtual Cam...
    regsvr32.exe /i /s "%~dp0..\data\obs-plugins\win-dshow\obs-virtualcam-module32.dll"
	reg query "HKLM\SOFTWARE\Classes\WOW6432Node\CLSID\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}" >nul 2>&1
	if %errorLevel% == 0 (
		echo 32-bit Virtual Cam successfully installed
		echo.
	) else (
		echo 32-bit Virtual Cam installation failed
		echo.
		goto endFail
	)
	goto checkDLLContinue

:install64DLL
	echo Installing 64-bit Virtual Cam...
    regsvr32.exe /i /s "%~dp0..\data\obs-plugins\win-dshow\obs-virtualcam-module64.dll"
	reg query "HKLM\SOFTWARE\Classes\CLSID\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}" >nul 2>&1
	if %errorLevel% == 0 (
		echo 64-bit Virtual Cam successfully installed
		echo.
		goto endSuccess
	) else (
		echo 64-bit Virtual Cam installation failed
		echo.
		goto endFail
	)

:endFail
    echo Something failed, please report this on the OBS Discord or Forums!
    pause

:endSuccess
	echo Virtual Cam installed!
	echo.

:end
    exit

:ResolvePath
    SET RETVAL=%~f1
    exit /b
