#!/usr/bin/env bash
# Build launcher/Burnout3Launcher.exe on Linux (recovery-era replacement for the lost
# tools/build_launcher.ps1). Uses the .NET SDK's C# compiler against the .NET
# Framework 4.8 reference assemblies; the result runs on Windows and under Wine.
#
# Needs: DOTNET (path to the dotnet executable) and NET48_REFS (folder with the
# .NET Framework 4.8 reference assemblies, from the NuGet package
# Microsoft.NETFramework.ReferenceAssemblies.net48).
# TOOLCHAIN may name a folder holding dotnet/, refs/net48/ and dotnet-home/ instead.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
toolchain="${TOOLCHAIN:-}"
if [ -z "${DOTNET:-}" ] && [ -z "$toolchain" ]; then
    echo "set DOTNET and NET48_REFS (or TOOLCHAIN)" >&2
    exit 2
fi
DOTNET="${DOTNET:-$toolchain/dotnet/dotnet}"
NET48_REFS="${NET48_REFS:-$toolchain/refs/net48/build/.NETFramework/v4.8}"
export DOTNET_CLI_HOME="${DOTNET_CLI_HOME:-$toolchain/dotnet-home}" DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
csc="$(find "$(dirname "$DOTNET")/sdk" -name csc.dll -path '*bincore*' | head -1)"
out="${1:-$here/build-launcher}"
mkdir -p "$out"
refs=()
for r in mscorlib System System.Core System.Xml System.Xaml WindowsBase PresentationCore \
         PresentationFramework System.Web.Extensions System.IO.Compression \
         System.IO.Compression.FileSystem System.Net.Http; do
    refs+=("-r:$NET48_REFS/$r.dll")
done
cd "$here/launcher"
"$DOTNET" "$csc" -nologo -langversion:5 -target:winexe -platform:x64 -optimize+ \
    -nostdlib -noconfig -nowarn:1701 "${refs[@]}" \
    -win32icon:assets/b3.ico \
    -resource:Window.xaml,Window.xaml \
    -out:"$out/Burnout3Launcher.exe" Program.cs Core.cs RuntimeSetup.cs
echo "built $out/Burnout3Launcher.exe"
