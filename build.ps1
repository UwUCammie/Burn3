#requires -Version 5.1
<#
.SYNOPSIS
    Configure, build, and test the takedown runtime on Windows.

.DESCRIPTION
    The runtime uses GCC/Clang language extensions (`__int128`,
    `__attribute__`, `__extension__`), so Windows builds use the MinGW-w64 GCC
    toolchain rather than MSVC. Put the MinGW-w64 compiler on PATH (for example
    MSYS2's `mingw64\bin`) before running this script.

    Dependencies (SDL3, zstd, FFmpeg) are resolved from vcpkg when VCPKG_ROOT is
    set, or from an MSYS2/MinGW prefix passed with -Prefix.

    -NativeModules validates the preserved local generated-source inventory and
    enables its reviewed module profile. Use a separate BuildDir for comparisons;
    an existing conflicting or custom module inventory is never replaced.

    Repeated Ninja builds reuse a successful configuration when arguments,
    tools, environment, and generated build graph match. Ninja still checks
    CMake inputs and generated-source globs. -Reconfigure forces a fresh CMake
    configure without deleting compiled objects.

.EXAMPLE
    ./build.ps1
    ./build.ps1 -Configuration Debug -NoTests
    ./build.ps1 -Prefix C:\msys64\mingw64
    ./build.ps1 -NativeModules -BuildDir build-native-windows -Target takedown-recomp -NoTests
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDir = 'build',
    [string]$Generator = 'Ninja',
    [string]$CxxCompiler = 'g++',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Triplet = 'x64-mingw-dynamic',
    [string]$Prefix,
    [string[]]$Target,
    [switch]$NativeModules,
    [string]$NativeModuleManifest = 'migration/native-build-profile.json',
    [string]$Vu1InlineModule,
    [string]$Vu1AotModule,
    [string]$EeAotModule,
    [string]$Vu0AotModule,
    [switch]$NoTests,
    [switch]$Fresh,
    [switch]$Reconfigure,
    # Optional persistent GCC LTO cache; supported toolchains are checked by CMake.
    [switch]$IncrementalLto,
    # ccache is used as the compiler launcher when it is on PATH (it makes a
    # rebuild after a reconfigure, a branch switch or a clean build directory
    # cheap, which matters most for the EE AOT shards); this turns that off.
    [switch]$NoCcache,
    # Compile jobs (also the LTO link partitions). The default leaves half the
    # logical processors free so the machine stays usable while it builds.
    [ValidateRange(1, 256)]
    [int]$Jobs = [Math]::Max(2, [int][Math]::Floor([Environment]::ProcessorCount / 2))
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $repoRoot
try {
    # Prefer a toolchain already on PATH; otherwise fall back to a standard MSYS2
    # install so `run.bat` works from a plain PowerShell or cmd shell. This also
    # puts the MinGW runtime DLLs on PATH for the launched game.
    if (-not (Get-Command $CxxCompiler -ErrorAction SilentlyContinue)) {
        $searchDirs = @(
            'C:\msys64\mingw64\bin',
            'C:\msys64\ucrt64\bin',
            (Join-Path $env:ProgramFiles 'msys64\mingw64\bin')
        )
        foreach ($dir in $searchDirs) {
            if (Test-Path (Join-Path $dir "$CxxCompiler.exe")) {
                $env:PATH = "$dir;$env:PATH"
                Write-Host "Using MinGW-w64 toolchain from $dir" -ForegroundColor DarkGray
                break
            }
        }
    }

    if (-not (Get-Command $CxxCompiler -ErrorAction SilentlyContinue)) {
        throw "C++ compiler '$CxxCompiler' was not found on PATH. Install MinGW-w64 (for example via MSYS2) and add its bin directory to PATH."
    }

    $configureArgs = @(
        '-S', $repoRoot,
        '-B', $BuildDir,
        '-G', $Generator,
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_CXX_COMPILER=$CxxCompiler",
        "-DTAKEDOWN_LTO_JOBS=$Jobs"
    )

    $ccache = if ($NoCcache) { $null } else { Get-Command ccache -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if ($ccache) {
        Write-Host "Using ccache from $($ccache.Source)" -ForegroundColor DarkGray
        $configureArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$($ccache.Source)"
    } elseif ($NoCcache) {
        # Omitting the argument would retain a launcher from CMakeCache.txt.
        $configureArgs += '-DCMAKE_CXX_COMPILER_LAUNCHER:STRING='
    }

    if ($IncrementalLto) {
        $configureArgs += '-DTAKEDOWN_ENABLE_INCREMENTAL_LTO=ON'
    }

    if ($Vu1InlineModule) {
        $inlineModulePath = (Resolve-Path -LiteralPath $Vu1InlineModule).ProviderPath
        $configureArgs += "-DTAKEDOWN_LOCAL_VU1_INLINE_MODULE:FILEPATH=$inlineModulePath"
    }
    if ($Vu1AotModule) {
        # The ahead-of-time VU1 translation replaces the older inline-block
        # module; clear that one so an existing build tree stops compiling it.
        $aotModulePath = (Resolve-Path -LiteralPath $Vu1AotModule).ProviderPath
        $configureArgs += "-DTAKEDOWN_LOCAL_VU1_AOT_MODULE:FILEPATH=$aotModulePath"
        if (-not $Vu1InlineModule) {
            $configureArgs += '-DTAKEDOWN_LOCAL_VU1_INLINE_MODULE:FILEPATH='
        }
    }

    if ($Vu0AotModule) {
        # Ahead-of-time compiled VU0 microprograms (tools/vu0_aot_generator.py).
        $vu0ModulePath = (Resolve-Path -LiteralPath $Vu0AotModule).ProviderPath
        $configureArgs += "-DTAKEDOWN_LOCAL_VU0_AOT_MODULE:FILEPATH=$vu0ModulePath"
    }

    if ($EeAotModule) {
        # Ahead-of-time compiled game functions (tools/ee_aot_generator.cpp);
        # the shards beside the module are picked up by CMake.
        $eeModulePath = (Resolve-Path -LiteralPath $EeAotModule).ProviderPath
        $configureArgs += "-DTAKEDOWN_LOCAL_EE_AOT_MODULE:FILEPATH=$eeModulePath"
    }

    if ($NativeModules) {
        # Prefer the compiler's companion Python, then actual executables on
        # PATH. Store aliases and shell functions are not usable interpreters;
        # a broken candidate must not hide a working later installation.
        $pythonCandidates = @()
        $compilerApplication = Get-Command $CxxCompiler -CommandType Application `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($compilerApplication) {
            $compilerDirectory = Split-Path -Parent $compilerApplication.Source
            $pythonCandidates += (Join-Path $compilerDirectory 'python3.exe')
            $pythonCandidates += (Join-Path $compilerDirectory 'python.exe')
        }
        $pythonCandidates += @(Get-Command python3, python -All -CommandType Application `
            -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
        $python = $null
        foreach ($candidate in ($pythonCandidates | Select-Object -Unique)) {
            if (-not (Test-Path -LiteralPath $candidate -PathType Leaf) -or
                $candidate -match '(?i)[\\/]WindowsApps[\\/]') {
                continue
            }
            try {
                $probeOutput = & $candidate -c "import json, sys; print(json.dumps({'executable': sys.executable, 'version': list(sys.version_info[:2])}))" 2>$null
                if ($LASTEXITCODE -ne 0) { continue }
                $probe = ($probeOutput -join "`n") | ConvertFrom-Json
                if ($probe.version.Count -ne 2 -or [int]$probe.version[0] -ne 3 -or
                    [int]$probe.version[1] -lt 10 -or
                    -not (Test-Path -LiteralPath $probe.executable -PathType Leaf) -or
                    $probe.executable -match '(?i)[\\/]WindowsApps[\\/]') {
                    continue
                }
                $python = [IO.Path]::GetFullPath($probe.executable)
                break
            } catch {
                # Access-denied, missing-DLL, and malformed-output candidates
                # are skipped so another installed interpreter can be used.
                continue
            }
        }
        if (-not $python) {
            throw '-NativeModules requires a working Python 3.10+ executable (install it beside MinGW or add it to PATH).'
        }
        $validation = & $python -B (Join-Path $repoRoot 'tools\windows_native_module_profile.py') `
            --source-dir $repoRoot --build-dir $BuildDir --manifest $NativeModuleManifest
        if ($LASTEXITCODE -ne 0) { throw 'Native module validation failed; build configuration was not changed.' }
        $nativeProfile = ($validation -join "`n") | ConvertFrom-Json
        $configureArgs += @($nativeProfile.configure_arguments)
        $configureArgs += "-DPython3_EXECUTABLE:FILEPATH=$python"
        Write-Host "Validated $($nativeProfile.configured_modules) local modules / $($nativeProfile.total_generated_handlers) handlers." -ForegroundColor DarkGray
    }

    if ($VcpkgRoot) {
        $toolchain = Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'
        if (-not (Test-Path $toolchain)) {
            throw "vcpkg toolchain not found at $toolchain"
        }
        $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
        $configureArgs += "-DVCPKG_TARGET_TRIPLET=$Triplet"
    }
    if ($Prefix) {
        $configureArgs += "-DCMAKE_PREFIX_PATH=$Prefix"
    }

    if ($Fresh -and (Test-Path $BuildDir)) {
        $resolvedBuildDir = (Resolve-Path -LiteralPath $BuildDir).ProviderPath
        $resolvedRepoRoot = (Resolve-Path -LiteralPath $repoRoot).ProviderPath.TrimEnd('\', '/')
        $repoPrefix = $resolvedRepoRoot + [IO.Path]::DirectorySeparatorChar
        if (-not $resolvedBuildDir.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw '-Fresh can only remove a build directory inside this repository, never the repository root.'
        }
        # A junction in the path can redirect an apparently local build outside
        # the repository. Refuse it before any recursive filesystem operation.
        $checkedPath = $resolvedBuildDir
        while ($checkedPath.Length -gt $resolvedRepoRoot.Length) {
            $item = Get-Item -LiteralPath $checkedPath -Force
            if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw '-Fresh refuses files, symbolic links, and junctions; select an ordinary build directory.'
            }
            $checkedPath = Split-Path -Parent $checkedPath
        }
        Remove-Item -LiteralPath $resolvedBuildDir -Recurse -Force
    }

    $configureCacheHelper = Join-Path $repoRoot 'tools/windows_build_configure_cache.ps1'
    . $configureCacheHelper
    $configureIdentity = $null
    if ($Generator -eq 'Ninja') {
        try {
            $toolPaths = @($MyInvocation.MyCommand.Path, $configureCacheHelper)
            foreach ($name in @('cmake', $CxxCompiler, 'ninja')) {
                $tool = Get-Command $name -CommandType Application -ErrorAction Stop |
                    Select-Object -First 1
                $toolPaths += $tool.Source
            }
            if ($ccache) { $toolPaths += $ccache.Source }
            $configureIdentity = Get-TakedownConfigureIdentity -Arguments $configureArgs -ToolPaths $toolPaths
        } catch {
            Write-Host "Configure cache unavailable; configuring normally: $_" -ForegroundColor DarkGray
        }
    }
    if (-not $Fresh -and -not $Reconfigure -and $configureIdentity -and
        (Test-TakedownConfigureCache -BuildDir $BuildDir -Identity $configureIdentity)) {
        Write-Host 'Reusing CMake configuration; Ninja will check build inputs.' -ForegroundColor DarkGray
    } else {
        # A failed configure must never leave a previously valid reuse record.
        Clear-TakedownConfigureCache -BuildDir $BuildDir
        Write-Host "Configuring ($Generator, $CxxCompiler)..." -ForegroundColor Cyan
        cmake @configureArgs
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
        if ($configureIdentity) {
            Save-TakedownConfigureCache -BuildDir $BuildDir -Identity $configureIdentity
        }
    }

    Write-Host "Building ($Configuration, $Jobs jobs, below-normal priority)..." -ForegroundColor Cyan
    $buildArgs = @('--build', $BuildDir, '--config', $Configuration, '--parallel', $Jobs)
    if ($Target) { $buildArgs += @('--target') + $Target }
    # Compilers inherit this process's priority class, so foreground work on
    # the machine keeps priority over the build.
    $self = Get-Process -Id $PID
    $previousPriority = $self.PriorityClass
    try {
        $self.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
        cmake @buildArgs
    } finally {
        $self.PriorityClass = $previousPriority
    }
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

    # Ninja may have regenerated successfully after a CMake input edit. Save
    # that graph too, so the next build does not repeat the same configuration.
    if ($configureIdentity) {
        Save-TakedownConfigureCache -BuildDir $BuildDir -Identity $configureIdentity
    }

    if (-not $NoTests) {
        Write-Host "Testing ($Configuration)..." -ForegroundColor Cyan
        ctest --test-dir $BuildDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
    }

    Write-Host "Build complete: $BuildDir" -ForegroundColor Green
} finally {
    Pop-Location
}
