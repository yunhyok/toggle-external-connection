param([ValidateSet('Win7','Win10-11')][string]$Target='Win7', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root "build/$Target"
New-Item -ItemType Directory -Force -Path $out | Out-Null
function Invoke-Checked($tool, $arguments) {
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) { throw "$tool exited with $LASTEXITCODE" }
}
Push-Location $out
try {
    $legacy = $Target -eq 'Win7'
    $win = if ($legacy) {'0x0601'} else {'0x0A00'}
    $sub = if ($legacy) {'6.01'} else {'10.00'}
    $flags = @('/nologo','/std:c++17','/EHsc','/utf-8','/O2','/MT','/W4','/DNDEBUG',
        '/DUNICODE','/D_UNICODE',"/D_WIN32_WINNT=$win","/DWINVER=$win","/I$root/src")
    $libs = @('fwpuclnt.lib','rpcrt4.lib','ole32.lib','uuid.lib','user32.lib','shell32.lib',
        'comctl32.lib','gdi32.lib','iphlpapi.lib','ws2_32.lib','dnsapi.lib','advapi32.lib',
        'uxtheme.lib','shlwapi.lib','comdlg32.lib','version.lib')
    $sources = @("$root/src/settings.cpp","$root/src/engine.cpp","$root/src/history.cpp","$root/src/install_path.cpp")
    Invoke-Checked cl.exe ($flags + @("$root/src/install_path.cpp","$root/src/install_guard.cpp",'/Fe:install-guard.exe',
        '/link',"/SUBSYSTEM:CONSOLE,$sub",'/MACHINE:X64','advapi32.lib','version.lib'))
    Invoke-Checked rc.exe @('/nologo','/I',"$root/assets",'/fo','app.res',"$root/assets/app.rc")
    Invoke-Checked cl.exe ($flags + $sources + @("$root/src/ui.cpp",'/Fe:ToggleExternalConnection.exe',
        '/link',"/SUBSYSTEM:WINDOWS,$sub",'/MACHINE:X64','/MANIFEST:NO','/DYNAMICBASE','/NXCOMPAT','app.res') + $libs)
    if (!$SkipTests) {
        $checks = @(Get-ChildItem -LiteralPath "$root/tests" -Filter '*.cpp' | ForEach-Object FullName)
        Invoke-Checked cl.exe ($flags + @('/DTEC_TESTING') + $sources + $checks +
            @('/Fe:offline-tests.exe','/link',"/SUBSYSTEM:CONSOLE,$sub",'/MACHINE:X64') + $libs)
        Invoke-Checked '.\offline-tests.exe' @()
    }
} finally { Pop-Location }
