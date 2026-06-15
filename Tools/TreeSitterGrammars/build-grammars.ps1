<#
.SYNOPSIS
    Downloads and compiles tree-sitter grammar DLLs for WinMerge.
.DESCRIPTION
    Reads grammars.json, downloads release tarballs from GitHub, reads each
    repo's tree-sitter.json for source layout, compiles to DLL via MSVC cl.exe.
.PARAMETER OutDir
    Output directory for DLLs and .scm files.
.PARAMETER Platform
    Target platform: x64, x86, or ARM64. Default: x64.
.PARAMETER Configuration
    Build configuration: Release or Debug. Default: Release.
.PARAMETER GrammarFilter
    Optional regex to build only matching grammar names.
.EXAMPLE
    .\build-grammars.ps1
    .\build-grammars.ps1 -GrammarFilter "fsharp"
#>
param(
    [string]$OutDir,
    [ValidateSet("x64","x86","ARM64")]
    [string]$Platform = "x64",
    [ValidateSet("Release","Debug")]
    [string]$Configuration = "Release",
    [string]$GrammarFilter
)

$ErrorActionPreference = "Continue"

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
if (-not $OutDir) {
    $OutDir = Join-Path $RepoRoot "Build\$Platform\$Configuration\TreeSitterGrammars"
}
$TempBase   = Join-Path $RepoRoot "BuildTmp\grammar-sources"
$ConfigFile = Join-Path $ScriptDir "grammars.json"

# ---- Locate and import MSVC environment ----

function Find-VcVarsAll {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        $vswhere = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
    }
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }
    $ip = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $ip) { throw "No VS with C++ tools found" }
    $vcvars = Join-Path $ip "VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars" }
    return $vcvars
}

function Import-VcEnvironment {
    param([string]$Arch)
    $vcvars = Find-VcVarsAll
    Write-Host "Importing MSVC environment ($Arch) ..."
    $batContent = "@call `"$vcvars`" $Arch >nul 2>&1`r`n@set > `"$env:TEMP\vcvars_env.txt`"`r`n"
    [System.IO.File]::WriteAllText("$env:TEMP\vcvars_capture.bat", $batContent)
    cmd.exe /c "$env:TEMP\vcvars_capture.bat"
    if (Test-Path "$env:TEMP\vcvars_env.txt") {
        foreach ($line in (Get-Content "$env:TEMP\vcvars_env.txt")) {
            if ($line -match '^([^=]+)=(.*)$') {
                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
            }
        }
        Remove-Item "$env:TEMP\vcvars_capture.bat","$env:TEMP\vcvars_env.txt" -Force -EA SilentlyContinue
    }
    $cl = Get-Command cl.exe -EA SilentlyContinue
    if (-not $cl) { throw "cl.exe not found after importing vcvars" }
    Write-Host "  cl.exe: $($cl.Source)"
}

function Get-VcArch {
    switch ($Platform) {
        "x64"   { return "amd64" }
        "x86"   { return "x86" }
        "ARM64" { return "amd64_arm64" }
        default { return "amd64" }
    }
}

# ---- Download grammar source ----

# Extract a GitHub auto-generated source zip, stripping the leading
# "<repo>-<tag>/" path component so the layout matches a release tarball.
function Expand-SourceZip {
    param([string]$ZipPath, [string]$DestDir)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($entry in $zip.Entries) {
            $rel = $entry.FullName
            $slash = $rel.IndexOf('/')
            if ($slash -ge 0) { $rel = $rel.Substring($slash + 1) }
            if (-not $rel) { continue }
            $dest = Join-Path $DestDir ($rel -replace '/', '\')
            if ($entry.FullName.EndsWith('/')) {
                New-Item -ItemType Directory -Path $dest -Force | Out-Null
                continue
            }
            $parent = Split-Path -Parent $dest
            if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
        }
    } finally {
        $zip.Dispose()
    }
}

function Get-GrammarSource {
    param([string]$Repo, [string]$Tag)
    $name = ($Repo -split '/')[-1]
    $extractDir = Join-Path $TempBase $name
    if (Test-Path (Join-Path $extractDir "tree-sitter.json")) {
        Write-Host "  Cached: $extractDir"
        return $extractDir
    }
    New-Item -ItemType Directory -Path $TempBase -Force | Out-Null
    New-Item -ItemType Directory -Path $extractDir -Force | Out-Null

    # Prefer a release tarball (.tar.gz then .tar.xz); extract with git-bash tar
    # (Windows tar.exe trips on ./-prefixed entries).
    $downloaded = $false
    foreach ($ext in @("tar.gz", "tar.xz")) {
        $tarUrl  = "https://github.com/$Repo/releases/download/$Tag/$name.$ext"
        $tarFile = Join-Path $TempBase "$name.$ext"
        try {
            Write-Host "  Downloading $name.$ext ..."
            Invoke-WebRequest -Uri $tarUrl -OutFile $tarFile -UseBasicParsing -ErrorAction Stop
            Write-Host "  Extracting ..."
            $gitBash = Join-Path $env:ProgramFiles "Git\bin\bash.exe"
            $unixTarFile = $tarFile -replace '\\','/' -replace '^([A-Za-z]):','/$1'
            $unixExtractDir = $extractDir -replace '\\','/' -replace '^([A-Za-z]):','/$1'
            & $gitBash -c "tar -xf '$unixTarFile' -C '$unixExtractDir'"
            Remove-Item $tarFile -Force -EA SilentlyContinue
            $downloaded = $true
            break
        } catch {
            Remove-Item $tarFile -Force -EA SilentlyContinue
        }
    }

    # Fallback: GitHub's auto-generated source zip, which exists for every tag
    # (many grammar repos publish no release tarball assets at all).
    if (-not $downloaded) {
        $zipUrl  = "https://github.com/$Repo/archive/refs/tags/$Tag.zip"
        $zipFile = Join-Path $TempBase "$name-src.zip"
        try {
            Write-Host "  Downloading source zip ($Tag) ..."
            Invoke-WebRequest -Uri $zipUrl -OutFile $zipFile -UseBasicParsing -ErrorAction Stop
            Write-Host "  Extracting source zip ..."
            Expand-SourceZip -ZipPath $zipFile -DestDir $extractDir
            Remove-Item $zipFile -Force -EA SilentlyContinue
            $downloaded = $true
        } catch {
            Remove-Item $zipFile -Force -EA SilentlyContinue
        }
    }

    if (-not $downloaded) {
        throw "No release tarball or source zip found for $Repo $Tag"
    }
    return $extractDir
}

function Resolve-NodeModulesPath {
    param(
        [string]$Candidate,
        [string]$CacheBaseDir
    )

    $separator = [string][IO.Path]::DirectorySeparatorChar
    $escapedSeparator = [regex]::Escape($separator)
    $normalized = $Candidate -replace '/', $separator
    if (-not $normalized.StartsWith("node_modules$separator")) {
        return $null
    }

    $parts = $normalized -split $escapedSeparator
    $isScopedPackage = ($parts.Count -ge 2 -and $parts[1].StartsWith('@'))
    $minPartCount = if ($isScopedPackage) { 4 } else { 3 }
    if ($parts.Count -lt $minPartCount) {
        return $null
    }

    $packageName = $parts[1]
    $restIndex = 2
    $cacheDirs = New-Object System.Collections.Generic.List[string]
    if ($isScopedPackage) {
        $scopedPackageName = $packageName + $separator + $parts[2]
        $restIndex = 3
        $cacheDirs.Add((Join-Path $CacheBaseDir $scopedPackageName))
        $packageNameWithinScope = $parts[2]
    } else {
        $packageNameWithinScope = $packageName
    }
    $cacheDirs.Add((Join-Path $CacheBaseDir $packageNameWithinScope))

    if ($restIndex -ge $parts.Count) {
        return $null
    }
    $relativePath = $parts[$restIndex..($parts.Count - 1)] -join $separator
    foreach ($cachedSourceDir in $cacheDirs) {
        if (-not (Test-Path $cachedSourceDir)) {
            continue
        }
        $resolvedPath = Join-Path $cachedSourceDir $relativePath
        if (Test-Path $resolvedPath) {
            return $resolvedPath
        }
    }

    return $null
}

function Resolve-QueryFiles {
    param(
        [object]$QuerySpec,
        [string]$SourceDir,
        [string]$RepoDir,
        [string]$CacheBaseDir,
        [string]$FallbackRelativePath
    )

    $resolved = New-Object System.Collections.Generic.List[string]

    if ($QuerySpec) {
        $queryPaths = if ($QuerySpec -is [array]) { $QuerySpec } else { @($QuerySpec) }
        foreach ($candidate in $queryPaths) {
            # $PSScriptRoot lets a spec point at this repo's own vendored-queries\
            # directory (for grammars whose upstream ships no usable .scm), e.g.
            # "vendored-queries\python-locals.scm".
            $tryPaths = @(
                (Join-Path $RepoDir $candidate),
                (Join-Path $SourceDir $candidate),
                (Join-Path $PSScriptRoot $candidate),
                (Resolve-NodeModulesPath -Candidate $candidate -CacheBaseDir $CacheBaseDir)
            ) | Where-Object { $_ }

            foreach ($tryPath in $tryPaths) {
                if ((Test-Path $tryPath) -and (-not $resolved.Contains($tryPath))) {
                    $resolved.Add($tryPath)
                    break
                }
            }
        }
    }

    if ($resolved.Count -eq 0 -and $FallbackRelativePath) {
        $fallbacks = @(
            (Join-Path $SourceDir $FallbackRelativePath),
            (Join-Path $RepoDir $FallbackRelativePath)
        )
        foreach ($fallback in $fallbacks) {
            if ((Test-Path $fallback) -and (-not $resolved.Contains($fallback))) {
                $resolved.Add($fallback)
                break
            }
        }
    }

    return $resolved.ToArray()
}

function Write-QueryBundle {
    param(
        [string]$DestinationPath,
        [string[]]$SourcePaths
    )

    if (-not $SourcePaths -or $SourcePaths.Count -eq 0) {
        return $false
    }

    $contents = foreach ($sourcePath in $SourcePaths) {
        try {
            [System.IO.File]::ReadAllText($sourcePath)
        } catch {
            Write-Warning "  Failed to read query source '$sourcePath': $_"
            return $false
        }
    }
    if ($contents -contains $false) {
        return $false
    }
    $newline = [System.Environment]::NewLine
    $doubleNewline = $newline + $newline
    $bundle = ($contents -join $doubleNewline).TrimEnd()
    [System.IO.File]::WriteAllText($DestinationPath, $bundle + $newline, [System.Text.UTF8Encoding]::new($false))
    return $true
}

# ---- Parser generation (for grammars that don't ship src\parser.c) ----

# A few grammars publish only grammar.js in their release tag and expect the
# consumer to run `tree-sitter generate`. Do that on demand. Requires Node/npm on
# PATH; if generation isn't possible the grammar is simply skipped by the caller.
function Ensure-GeneratedParser {
    param([string]$GrammarName, [string]$GrammarSourceDir)

    $parserPath = Join-Path $GrammarSourceDir "src\parser.c"
    if (Test-Path $parserPath) {
        return $true
    }

    $grammarJsPath = Join-Path $GrammarSourceDir "grammar.js"
    if (-not (Test-Path $grammarJsPath)) {
        return $false
    }

    Write-Host "  Generating parser sources for $GrammarName ..."
    # Invoke through cmd.exe so PATHEXT resolves npm -> npm.cmd. A bare "npm" via
    # Start-Process can hit the extensionless Unix shell script shipped alongside
    # node ("%1 is not a valid Win32 application").
    Push-Location $GrammarSourceDir
    try {
        & cmd.exe /c npm exec --yes tree-sitter-cli -- generate 2>&1 | Write-Host
        $exit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($exit -ne 0) {
        Write-Warning "  tree-sitter generate failed for $GrammarName (exit $exit) - skipping"
        return $false
    }

    return (Test-Path $parserPath)
}

# ---- Up-to-date check ----

# A grammar needs (re)building when its DLL is missing or older than any input
# (the generated sources or this script). Query .scm files are bundled
# separately and aren't compiled into the DLL, so they're not inputs here.
function Test-NeedsBuild {
    param([string]$DllPath, [string[]]$Inputs)
    if (-not (Test-Path $DllPath)) { return $true }
    $dllTime = (Get-Item $DllPath).LastWriteTimeUtc
    foreach ($in in $Inputs) {
        if ($in -and (Test-Path $in)) {
            if ((Get-Item $in).LastWriteTimeUtc -gt $dllTime) { return $true }
        }
    }
    return $false
}

# ---- Parallel compilation ----

# Self-contained compile+link for one grammar. Runs inside a background job, so
# it may use only its $Plan argument, the inherited MSVC environment, and
# built-in cmdlets (no script-level functions/variables).
$BuildGrammarScript = {
    param($Plan)
    New-Item -ItemType Directory -Path $Plan.BuildDir -Force | Out-Null
    $objs = @()
    foreach ($src in $Plan.Sources) {
        $obj = Join-Path $Plan.BuildDir ([IO.Path]::GetFileNameWithoutExtension($src) + ".obj")
        $objs += $obj
        # CFlags forces C with /TC; C++ scanners (.cc/.cpp/.cxx, e.g. hcl) need /TP.
        # cl applies the last /T* flag, so appending /TP overrides /TC for those files.
        $langFlag = @()
        if ($src -match '\.(cc|cpp|cxx)$') { $langFlag = @("/TP") }
        $clArgs = @($Plan.CFlags) + $langFlag + @("/I$($Plan.SrcDir)", "/Fo$obj", "$src")
        $out = & cl.exe @clArgs 2>&1
        if ($LASTEXITCODE -ne 0) {
            return [pscustomobject]@{ Name = $Plan.Name; Ok = $false; Stage = "compile $([IO.Path]::GetFileName($src))"; Output = ($out -join [Environment]::NewLine) }
        }
    }
    $linkArgs = @($Plan.LinkFlags) + @("/OUT:$($Plan.DllPath)") + $objs
    $out = & link.exe @linkArgs 2>&1
    return [pscustomobject]@{ Name = $Plan.Name; Ok = ($LASTEXITCODE -eq 0); Stage = "link"; Output = ($out -join [Environment]::NewLine) }
}

# Run build plans with bounded parallelism using background jobs (PowerShell 5.1
# compatible; ForEach-Object -Parallel needs pwsh 7, which the build doesn't use).
function Invoke-GrammarBuilds {
    param([scriptblock]$Action, [object[]]$Plans, [int]$Throttle)
    $results = @()
    if (-not $Plans -or $Plans.Count -eq 0) { return $results }
    if ($Throttle -lt 1) { $Throttle = 1 }
    $queue = New-Object System.Collections.Queue
    foreach ($pl in $Plans) { [void]$queue.Enqueue($pl) }
    $jobs = New-Object System.Collections.ArrayList
    while ($queue.Count -gt 0 -or $jobs.Count -gt 0) {
        while ($jobs.Count -lt $Throttle -and $queue.Count -gt 0) {
            [void]$jobs.Add((Start-Job -ScriptBlock $Action -ArgumentList $queue.Dequeue()))
        }
        $finished = Wait-Job -Job ($jobs.ToArray()) -Any
        foreach ($j in @($finished)) {
            $results += Receive-Job -Job $j
            Remove-Job -Job $j
            $jobs.Remove($j)
        }
    }
    return $results
}

# ---- Main ----

$succeeded = 0; $failed = 0; $skipped = 0; $cached = 0

Write-Host "=== WinMerge Tree-Sitter Grammar Builder ==="
Write-Host "Platform:      $Platform"
Write-Host "Configuration: $Configuration"
Write-Host "Output:        $OutDir"
Write-Host ""

Import-VcEnvironment -Arch (Get-VcArch)
Write-Host ""

# Shared compiler/linker flags. /GL (whole-program opt) and /LTCG are
# intentionally omitted: tree-sitter parser.c is mostly generated state tables,
# so they add large build time for negligible runtime benefit.
$cflags    = @("/nologo","/c","/TC","/W3","/D_USRDLL","/D_WINDOWS","/wd4996","/wd4267","/wd4244","/wd4101")
$linkFlags = @("/nologo","/DLL")
if ($Configuration -eq "Release") {
    $cflags    += @("/O2","/MD","/DNDEBUG")
    $linkFlags += @("/OPT:REF","/OPT:ICF")
} else {
    $cflags    += @("/Od","/MDd","/D_DEBUG","/Zi")
    $linkFlags += @("/DEBUG")
}

$config = Get-Content $ConfigFile -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# Phase 1 (sequential, fast): download sources, bundle query files, and decide
# which grammars actually need recompiling.
$plans = @()
foreach ($entry in $config.grammars) {
    $repo = $entry.repo; $tag = $entry.tag
    $repoName = ($repo -split '/')[-1]
    Write-Host "--- $repoName ($tag) ---"

    try {
        $repoDir = Get-GrammarSource -Repo $repo -Tag $tag
    } catch {
        Write-Warning "  Download failed: $_"
        $failed++
        continue
    }

    # Most grammars ship a tree-sitter.json describing their grammar name(s) and
    # source layout. Some older/popular ones (e.g. kotlin, erlang, solidity) don't;
    # for those, grammars.json may supply an explicit "name" and we assume the
    # conventional single-grammar layout (sources in ./src, queries in ./queries).
    $tsJsonPath = Join-Path $repoDir "tree-sitter.json"
    if (Test-Path $tsJsonPath) {
        $tsJson = Get-Content $tsJsonPath -Raw | ConvertFrom-Json
        $grammarList = $tsJson.grammars
    } elseif ($entry.name) {
        Write-Host "  No tree-sitter.json; using name '$($entry.name)' from grammars.json"
        $grammarList = @([pscustomobject]@{ name = $entry.name; path = "."; highlights = $null; locals = $null; tags = $null; injections = $null })
    } else {
        Write-Warning "  No tree-sitter.json and no 'name' in grammars.json - skipping"
        $skipped++
        continue
    }

    foreach ($g in $grammarList) {
        $gName = $g.name
        $gPath = $g.path
        if (-not $gPath) { $gPath = "." }

        if ($GrammarFilter -and ($gName -notmatch $GrammarFilter)) {
            $skipped++
            continue
        }

        $sourceDir = if ($gPath -eq ".") { $repoDir } else { Join-Path $repoDir $gPath }
        $dllName   = "tree-sitter-$gName"
        $srcDir    = Join-Path $sourceDir "src"
        $parserC   = Join-Path $srcDir "parser.c"
        if (-not (Test-Path $parserC)) {
            # Some grammars ship only grammar.js; try generating parser.c (needs Node/npm).
            [void](Ensure-GeneratedParser -GrammarName $gName -GrammarSourceDir $sourceDir)
        }
        if (-not (Test-Path $parserC)) {
            Write-Warning "  parser.c not found for $gName - skipping"
            $skipped++
            continue
        }
        $sources = @($parserC)
        # External scanners ship as scanner.c (C) or scanner.cc/.cpp (C++, e.g. hcl).
        foreach ($scannerName in @("scanner.c", "scanner.cc", "scanner.cpp")) {
            $scannerPath = Join-Path $srcDir $scannerName
            if (Test-Path $scannerPath) { $sources += $scannerPath; break }
        }

        # Resolve & bundle .scm query files (cheap; always refreshed). A grammars.json
        # entry may override any query set (e.g. point "locals" at a vendored-queries\
        # file) for grammars whose upstream repo ships no usable .scm or keeps it off
        # the conventional path; the per-entry override wins over the grammar's own
        # tree-sitter.json spec.
        $hlSpec  = if ($null -ne $entry.highlights)  { $entry.highlights }  else { $g.highlights }
        $locSpec = if ($null -ne $entry.locals)      { $entry.locals }      else { $g.locals }
        $tagSpec = if ($null -ne $entry.tags)        { $entry.tags }        else { $g.tags }
        $injSpec = if ($null -ne $entry.injections)  { $entry.injections }  else { $g.injections }
        $hlScm = Resolve-QueryFiles -QuerySpec $hlSpec  -SourceDir $sourceDir -RepoDir $repoDir -CacheBaseDir $TempBase -FallbackRelativePath "queries\highlights.scm"
        $lcScm = Resolve-QueryFiles -QuerySpec $locSpec -SourceDir $sourceDir -RepoDir $repoDir -CacheBaseDir $TempBase -FallbackRelativePath "queries\locals.scm"
        $tgScm = Resolve-QueryFiles -QuerySpec $tagSpec -SourceDir $sourceDir -RepoDir $repoDir -CacheBaseDir $TempBase -FallbackRelativePath "queries\tags.scm"
        $ijScm = Resolve-QueryFiles -QuerySpec $injSpec -SourceDir $sourceDir -RepoDir $repoDir -CacheBaseDir $TempBase -FallbackRelativePath "queries\injections.scm"
        if (-not (Write-QueryBundle -DestinationPath (Join-Path $OutDir "$gName-highlights.scm") -SourcePaths $hlScm)) {
            Write-Warning "  No highlights.scm for $gName"
        }
        [void](Write-QueryBundle -DestinationPath (Join-Path $OutDir "$gName-locals.scm")     -SourcePaths $lcScm)
        [void](Write-QueryBundle -DestinationPath (Join-Path $OutDir "$gName-tags.scm")       -SourcePaths $tgScm)
        [void](Write-QueryBundle -DestinationPath (Join-Path $OutDir "$gName-injections.scm") -SourcePaths $ijScm)

        $dllPath  = Join-Path $OutDir "$dllName.dll"
        $buildDir = Join-Path $RepoRoot "BuildTmp\grammar-build\$dllName\$Platform\$Configuration"
        if (-not (Test-NeedsBuild -DllPath $dllPath -Inputs (@($sources) + @($PSCommandPath)))) {
            Write-Host "  Up to date: $dllName.dll"
            $cached++
            continue
        }

        $plans += @{
            Name      = $dllName
            Sources   = $sources
            SrcDir    = $srcDir
            BuildDir  = $buildDir
            DllPath   = $dllPath
            CFlags    = $cflags
            LinkFlags = $linkFlags
        }
    }
    Write-Host ""
}

# Phase 2 (parallel): compile + link the grammars that need it.
if ($plans.Count -gt 0) {
    $throttle = [Environment]::ProcessorCount
    if ($throttle -lt 1) { $throttle = 1 }
    Write-Host "Building $($plans.Count) grammar(s) with up to $throttle parallel job(s) ..."
    foreach ($r in (Invoke-GrammarBuilds -Action $BuildGrammarScript -Plans $plans -Throttle $throttle)) {
        if ($r.Ok) {
            Write-Host "  OK: $($r.Name).dll"
            $succeeded++
        } else {
            Write-Warning "  FAILED ($($r.Stage)): $($r.Name)"
            if ($r.Output) { Write-Host $r.Output }
            $failed++
        }
    }
}

Write-Host ""
Write-Host "=== Done: $succeeded built, $cached up-to-date, $failed failed, $skipped skipped ==="
if ($failed -gt 0) { exit 1 }
