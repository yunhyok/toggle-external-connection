param([Parameter(Mandatory)][ValidateSet('Win7','Win10-11')][string]$Target)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Run installer smoke tests only on a disposable GitHub Actions runner.' }
$root = Split-Path $PSScriptRoot -Parent
$setup = Join-Path $root "dist/ToggleExternalConnection-$Target-x64-Setup.exe"
$tests = Join-Path $root "build/$Target/offline-tests.exe"
$guard = Join-Path $root "build/$Target/install-guard.exe"
$key = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{CA78C24A-7964-4F3A-8F40-DA64E9D82A11}_is1'
if (Test-Path $key) { throw 'The runner already has an installation; refusing to alter it.' }
$default = Join-Path $env:ProgramFiles 'Toggle External Connection'
$alternate = Join-Path ([IO.Path]::GetPathRoot($env:ProgramFiles)) "TEC Installer Smoke $env:GITHUB_RUN_ID\Alternate Folder"
$silent = '/SP-','/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART'
function Invoke-Setup([string]$Destination) {
    $arguments = $silent
    if ($Destination) { $arguments += "/DIR=`"$Destination`"" }
    (Start-Process $setup -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden).ExitCode
}
function Assert-Installed([string]$Destination) {
    $registered = (Get-ItemProperty $key).InstallLocation.TrimEnd('\')
    if ($registered -ne $Destination -or !(Test-Path "$Destination\ToggleExternalConnection.exe")) { throw 'Wrong installed destination.' }
    & $guard --check "$Destination\ToggleExternalConnection.exe"
    if ($LASTEXITCODE) { throw 'Installed executable is not protected.' }
}
if ($Target -eq 'Win7') {
    if (!(Invoke-Setup $alternate) -or (Test-Path $key) -or (Test-Path "$alternate\ToggleExternalConnection.exe")) { throw 'Legacy installer accepted the wrong OS.' }
    return
}
if (Invoke-Setup '') { throw 'Default installation failed.' }
Assert-Installed $default
& $tests --ci-install-path
if ($LASTEXITCODE) { throw 'Installed settings test failed.' }
$settingsHash = (Get-FileHash "$default\Data\settings.dat").Hash
# A journal fixture verifies byte preservation without running the policy engine.
$journal = 'history-20261103.log'
[IO.File]::WriteAllBytes("$default\Data\$journal", [byte[]](0..255))
$journalHash = (Get-FileHash "$default\Data\$journal").Hash
if (Invoke-Setup $default) { throw 'Same-folder upgrade failed.' }
if ((Get-FileHash "$default\Data\settings.dat").Hash -ne $settingsHash) { throw 'Same-folder upgrade changed settings.' }
if (Invoke-Setup $alternate) { throw 'Relocation failed.' }
Assert-Installed $alternate
if ((Test-Path "$default\ToggleExternalConnection.exe") -or (Test-Path "$default\Data\settings.dat")) { throw 'Previous installation was not retired.' }
if ((Get-FileHash "$alternate\Data\settings.dat").Hash -ne $settingsHash -or
    (Get-FileHash "$alternate\Data\$journal").Hash -ne $journalHash) { throw 'Relocation changed settings or history.' }
& $tests --ci-install-path
if ($LASTEXITCODE) { throw 'Relocated settings test failed.' }
# User-controlled ancestors and junctions must fail before copying app files.
$unsafe = Join-Path (Split-Path $alternate -Parent) 'Writable Parent'
New-Item -ItemType Directory -Path $unsafe -Force | Out-Null
$acl = Get-Acl $unsafe
$acl.SetOwner([System.Security.Principal.NTAccount]::new('BUILTIN','Administrators'))
$acl.AddAccessRule([System.Security.AccessControl.FileSystemAccessRule]::new(
    [System.Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'), 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
Set-Acl $unsafe $acl
if (!(Invoke-Setup "$unsafe\Rejected App") -or (Test-Path "$unsafe\Rejected App\ToggleExternalConnection.exe")) { throw 'Writable parent was accepted.' }
Assert-Installed $alternate
$linked = Join-Path (Split-Path $alternate -Parent) 'Linked Parent'
New-Item -ItemType Junction -Path $linked -Target $alternate | Out-Null
if (!(Invoke-Setup "$linked\Rejected App") -or (Test-Path "$alternate\Rejected App\ToggleExternalConnection.exe")) { throw 'Junction parent was accepted.' }
$unrelated = Join-Path (Split-Path $alternate -Parent) 'Unrelated Folder'
& $guard --prepare $unrelated
if ($LASTEXITCODE) { throw 'Could not create unrelated protected target fixture.' }
[IO.File]::WriteAllText("$unrelated\unrelated.txt", 'Must remain unchanged.')
$originalAcl = (Get-Acl $unrelated).Sddl
if (!(Invoke-Setup $unrelated) -or (Get-Acl $unrelated).Sddl -ne $originalAcl -or
    [IO.File]::ReadAllText("$unrelated\unrelated.txt") -ne 'Must remain unchanged.') { throw 'Unrelated target content or permissions changed.' }
$uninstaller = (Get-ChildItem "$alternate\unins*.exe").FullName
$run = Start-Process $uninstaller -ArgumentList $silent -Wait -PassThru -WindowStyle Hidden
if ($run.ExitCode -or (Test-Path "$alternate\ToggleExternalConnection.exe") -or
    (Test-Path "$alternate\Data\settings.dat") -or (Test-Path $key)) { throw 'Uninstall failed.' }
# Exercise the published rc.1 installer, not a rebuilt approximation of it.
$legacy = Join-Path $env:RUNNER_TEMP 'TEC-rc1-Setup.exe'
Invoke-WebRequest 'https://github.com/yunhyok/toggle-external-connection/releases/download/v0.1.0-rc.1/ToggleExternalConnection-Win10-11-x64-Setup.exe' -OutFile $legacy
if ((Get-FileHash $legacy).Hash -ne '59078c1a7c75cd5e3507b84a683a7a46c7ca92ac655dde9c1f32b898b0cca6f9') { throw 'Legacy installer checksum mismatch.' }
$run = Start-Process $legacy -ArgumentList $silent -Wait -PassThru -WindowStyle Hidden
if ($run.ExitCode -or !(Test-Path "$default\ToggleExternalConnection.exe")) { throw 'Legacy installation failed.' }
$legacyData = "$default\Data"
New-Item -ItemType Directory -Path $legacyData -Force | Out-Null
$legacyAcl = Get-Acl $legacyData
$legacyAcl.SetAccessRuleProtection($true,$false)
foreach ($sid in 'S-1-5-18','S-1-5-32-544') {
    $legacyAcl.AddAccessRule([System.Security.AccessControl.FileSystemAccessRule]::new(
        [System.Security.Principal.SecurityIdentifier]::new($sid), 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
}
Set-Acl $legacyData $legacyAcl
[IO.File]::WriteAllText("$legacyData\settings.dat", "TEC1`n{FE379A83-E678-405B-8581-6E2E7CAAA280}`nnet 192.0.2.0/24`n", [Text.UnicodeEncoding]::new($false,$false))
[IO.File]::WriteAllBytes("$legacyData\$journal", [byte[]](255..0))
$settingsHash = (Get-FileHash "$legacyData\settings.dat").Hash
$journalHash = (Get-FileHash "$legacyData\$journal").Hash
if (Invoke-Setup $alternate) { throw 'Legacy-to-rc.2 relocation failed.' }
Assert-Installed $alternate
if ((Test-Path "$default\ToggleExternalConnection.exe") -or
    (Get-FileHash "$alternate\Data\settings.dat").Hash -ne $settingsHash -or
    (Get-FileHash "$alternate\Data\$journal").Hash -ne $journalHash) { throw 'Legacy relocation did not preserve data and retire the old executable.' }
& $tests --ci-install-path
if ($LASTEXITCODE) { throw 'Legacy settings did not load after relocation.' }
$uninstaller = (Get-ChildItem "$alternate\unins*.exe").FullName
$run = Start-Process $uninstaller -ArgumentList $silent -Wait -PassThru -WindowStyle Hidden
if ($run.ExitCode -or (Test-Path $key) -or (Test-Path "$alternate\Data\settings.dat")) { throw 'Legacy-upgrade uninstall failed.' }
Write-Output 'PASS: OS guard, default/custom destinations, rc.2 and published rc.1 upgrades, data migration, unsafe/unrelated-path rejection and uninstall.'
