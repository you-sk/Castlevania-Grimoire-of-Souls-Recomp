param([switch]$ProbeOnly)
$ErrorActionPreference='Stop'
# PowerShell 7 parents may pass a module path that hides Windows PowerShell's
# built-in modules. Use the current engine's modules and Windows NetSecurity.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility') -Force
Import-Module (Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\Modules\NetSecurity') -Force
$root=$PSScriptRoot
$out=Join-Path $root ('output\run-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Path $out -Force | Out-Null
try {
 $manifest=Get-Content (Join-Path $root 'manifest.json') -Raw | ConvertFrom-Json
 foreach($entry in $manifest.PSObject.Properties){
  $path=Join-Path (Join-Path $root 'game') $entry.Name
  if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.Value){throw "Candidate hash mismatch: $($entry.Name)"}
 }
 $setup=Get-Content (Join-Path $root 'firewall-setup.json') -Raw | ConvertFrom-Json
 if(-not $setup.success){throw 'Firewall setup has not succeeded'}
 $profiles=@(Get-NetFirewallProfile -PolicyStore ActiveStore)
 if($profiles.Count -ne 3 -or @($profiles|Where-Object{-not $_.Enabled}).Count){throw 'Firewall not active on all profiles'}
 $expectedPrograms=@((Join-Path $root 'game\GoS.exe'),(Join-Path $root 'tools\isolated_launch.exe'),(Join-Path $root 'tools\network_probe.exe'))
 foreach($program in $expectedPrograms){foreach($direction in @('Inbound','Outbound')){
  $rules=@(Get-NetFirewallRule -PolicyStore ActiveStore -Group 'GoSRecomp-143-Isolated' | Where-Object {$_.Enabled -eq 'True' -and $_.Action -eq 'Block' -and $_.Direction -eq $direction -and $_.Profile -eq 'Any'} | Where-Object {($_|Get-NetFirewallApplicationFilter).Program -eq $program})
  if($rules.Count -ne 1){throw 'An effective per-program block rule is missing'}
  $port=$rules[0]|Get-NetFirewallPortFilter;$address=$rules[0]|Get-NetFirewallAddressFilter
  if($port.Protocol -ne 'Any' -or $address.RemoteAddress -ne 'Any'){throw 'Firewall rule scope changed'}
 }}
 $probeOut=Join-Path $out 'probe';New-Item -ItemType Directory -Path $probeOut | Out-Null
 & (Join-Path $root 'tools\isolated_launch.exe') (Join-Path $root 'tools\network_probe.exe') $probeOut probe
 if($LASTEXITCODE -ne 0){throw 'Network/child-process probe failed; refusing game launch'}
 $probe=Get-Content (Join-Path $probeOut 'launcher-game-output.txt') -Raw | ConvertFrom-Json
 if($probe.tcpConnectError -ne 10013 -or -not $probe.childDenied){throw 'Missing network or child-process denial proof'}
 @{firewallVerified=$true;childPolicyVerified=$true;gameFilesVerified=$true;probe=$probe;timeUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'preflight.json')
 if($ProbeOnly){Write-Output $out;exit 0}
 $save=Join-Path $root 'saves\quest2-3'
 if(-not(Test-Path $save -PathType Container)){throw 'Isolated save directory missing'}
 $env:GOSR_SAVE_ROOT=(Resolve-Path $save).Path
 $env:GOSR_GAME_IMAGE=Join-Path $root 'game\GameAssembly_arm64.dylib'
 $env:GOSR_SPFX_IMAGE=Join-Path $root 'game\GoS_Data\Plugins\x86_64\SPFXUnityPlugin.macho'
 $env:GOSR_SPFX_WORKERS='4';$env:GOSR_SPFX_TRACE_COINS='1';$env:GOSR_COIN_NOCULL='1';$env:GOSR_VERBOSE='1'
 & (Join-Path $root 'tools\isolated_launch.exe') (Join-Path $root 'game\GoS.exe') $out game
 @{exitCode=$LASTEXITCODE;saveRoot=$save;finishedUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $out 'result.json')
}catch{@{blocked=$true;error=$_.Exception.Message;timeUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $out 'blocked.json');throw}
