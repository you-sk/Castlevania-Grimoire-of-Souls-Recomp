param([Parameter(Mandatory=$true)][string]$HostRoot,[switch]$Remove)
$ErrorActionPreference='Stop'
$HostRoot=(Resolve-Path -LiteralPath $HostRoot).Path
$group='GoSRecomp-143-Isolated'
$log=Join-Path $HostRoot 'firewall-setup.json'
try {
 $principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
 if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator approval is required for these per-program rules'}
 $programs=@((Join-Path $HostRoot 'game\GoS.exe'),(Join-Path $HostRoot 'tools\isolated_launch.exe'),(Join-Path $HostRoot 'tools\network_probe.exe'))
 if(Test-Path (Join-Path $HostRoot 'game\UnityCrashHandler64.exe')){$programs+=(Join-Path $HostRoot 'game\UnityCrashHandler64.exe')}
 foreach($program in $programs){if(-not(Test-Path -LiteralPath $program -PathType Leaf)){throw "Missing program: $program"}}
 if($Remove){
  # Only this task's group and exact program paths can be removed.
  Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue | ForEach-Object {
   $filter=$_ | Get-NetFirewallApplicationFilter
   if($filter.Program -in $programs){$_ | Remove-NetFirewallRule}
  }
  @{removed=$true;group=$group} | ConvertTo-Json | Set-Content $log;exit 0
 }
 $profiles=@(Get-NetFirewallProfile -PolicyStore ActiveStore)
 if(@($profiles|Where-Object{-not $_.Enabled}).Count){throw 'A firewall profile is disabled; no global setting will be changed'}
 $i=0
 foreach($program in $programs){foreach($direction in @('Inbound','Outbound')){
  $name='GoSRecomp143-'+$i+'-'+$direction;$i++
  $existing=Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue
  if($existing){$filter=$existing|Get-NetFirewallApplicationFilter;if($filter.Program -ne $program){throw 'Rule name collision'}}
  else {New-NetFirewallRule -Name $name -DisplayName $name -Group $group -Program $program -Direction $direction -Action Block -Profile Any -Protocol Any -Enabled True | Out-Null}
 }}
 $proof=@()
 foreach($program in $programs){foreach($direction in @('Inbound','Outbound')){
  $rules=@(Get-NetFirewallRule -PolicyStore ActiveStore -Group $group | Where-Object {$_.Enabled -eq 'True' -and $_.Action -eq 'Block' -and $_.Direction -eq $direction -and $_.Profile -eq 'Any'} | Where-Object {($_|Get-NetFirewallApplicationFilter).Program -eq $program})
  if($rules.Count -ne 1){throw "Effective block rule missing: $program $direction"}
  $port=$rules[0]|Get-NetFirewallPortFilter
  $address=$rules[0]|Get-NetFirewallAddressFilter
  if($port.Protocol -ne 'Any' -or $address.RemoteAddress -ne 'Any'){throw 'Rule does not block all protocols/addresses'}
  $proof+=@{program=$program;direction=$direction;action='Block';profile='Any';protocol='Any';remoteAddress='Any'}
 }}
 @{success=$true;rules=$proof;globalSettingsChanged=$false;timeUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 5 | Set-Content $log
}catch{@{success=$false;error=$_.Exception.Message} | ConvertTo-Json | Set-Content $log;exit 1}
