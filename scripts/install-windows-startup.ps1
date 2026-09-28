param([Parameter(Mandatory = $true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$quartzPath = (Resolve-Path -LiteralPath $Executable).Path
if (!(Test-Path -LiteralPath $quartzPath -PathType Leaf) -or [IO.Path]::GetExtension($quartzPath) -ne '.exe') {
    throw 'Executable must name a built Quartz executable.'
}
$startupDirectory = [Environment]::GetFolderPath('Startup')
if (!$startupDirectory) { throw 'The current user Startup folder is unavailable.' }
[IO.Directory]::CreateDirectory($startupDirectory) | Out-Null
$shortcutPath = Join-Path $startupDirectory 'Quartz.lnk'
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
$shortcut.TargetPath = $quartzPath
$shortcut.Arguments = 'hidden'
$shortcut.WorkingDirectory = Split-Path $quartzPath -Parent
$shortcut.IconLocation = "$quartzPath,0"
$shortcut.Description = 'Quartz keyboard client. Show with Ctrl+Alt+Shift+Q.'
$shortcut.Save()
Write-Output "Quartz will start hidden when you sign in: $shortcutPath"
