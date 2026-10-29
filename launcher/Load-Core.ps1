if (-not ('Burn3Launcher.Core' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'Core.cs') -ReferencedAssemblies @(
        'System.dll', 'System.Core.dll', 'System.Web.Extensions.dll',
        'System.IO.Compression.dll', 'System.IO.Compression.FileSystem.dll'
    )
}
