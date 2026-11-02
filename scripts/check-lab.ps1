# Works with Windows PowerShell 2 on the separate lab PC. Never changes WFP or network settings.
param(
    [Parameter(Mandatory=$true)][ValidateSet('Before','Blocked','Restored')][string]$Phase,
    [Parameter(Mandatory=$true)][string]$InternalName,
    [Parameter(Mandatory=$true)][string]$ExternalIP,
    [Parameter(Mandatory=$true)][string]$ExternalDnsIP,
    [int]$InternalPort=443, [int]$ExternalPort=443,
    [string]$DnsTestName='example.com'
)
$ErrorActionPreference='Stop'
function Try-Tcp([Net.IPAddress]$Address, [int]$Port) {
    $socket=New-Object Net.Sockets.TcpClient($Address.AddressFamily)
    try {
        $result=$socket.BeginConnect($Address,$Port,$null,$null)
        if (!$result.AsyncWaitHandle.WaitOne(1800,$false)) { return $false }
        $socket.EndConnect($result)
        return $true
    } catch { return $false } finally { $socket.Close() }
}
function Safe-Name([string]$Value) {
    return $Value.Length -le 253 -and $Value -match '^[A-Za-z0-9][A-Za-z0-9.-]*$'
}
if (!(Safe-Name $DnsTestName) -or !(Safe-Name $InternalName)) { throw 'Use a DNS host name (ASCII or Punycode).' }
$outside=[Net.IPAddress]::Parse($ExternalIP)
$resolver=[Net.IPAddress]::Parse($ExternalDnsIP)
if ($ExternalPort -lt 1 -or $ExternalPort -gt 65535 -or $InternalPort -lt 1 -or $InternalPort -gt 65535) { throw 'Invalid TCP port.' }
$internalOk=$false
try {
    foreach ($address in [Net.Dns]::GetHostAddresses($InternalName)) {
        if (Try-Tcp $address $InternalPort) { $internalOk=$true; break }
    }
} catch { $internalOk=$false }
$externalOk=Try-Tcp $outside $ExternalPort
Write-Output ("Phase={0}  internal name/TCP={1}  external TCP={2}" -f $Phase,$internalOk,$externalOk)
Write-Output ("External TCP probe: powershell.exe -> {0} TCP/{1}" -f $outside,$ExternalPort)
Write-Output ("External DNS probe: nslookup.exe -> {0} UDP/53; compare answers and the application's history." -f $resolver)
# nslookup's exit status is unreliable on older Windows. Its output is evidence for manual comparison.
& "$env:SystemRoot\System32\nslookup.exe" '-timeout=1' '-retry=1' '-type=A' $DnsTestName $resolver.ToString()
if (!$internalOk -or ($externalOk -ne ($Phase -ne 'Blocked'))) { exit 1 }
exit 0
