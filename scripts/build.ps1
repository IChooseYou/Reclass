# PowerShell script to build Reclass
# Automatically detects Qt installation and configures build environment

#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory=$false)]
    [string]$QtDir,
    
    [Parameter(Mandatory=$false)]
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$BuildType = 'Release',
    
    [Parameter(Mandatory=$false)]
    [switch]$Clean,
    
    [Parameter(Mandatory=$false)]
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"

# ──────────────────────────────────────────────────────────────────────────────
# Functions
# ──────────────────────────────────────────────────────────────────────────────

function Write-ColorOutput {
    param([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::White)
    Write-Host $Message -ForegroundColor $Color
}

function Find-QtInstallation {
    $found = @()
    
    # Query registry for Qt installations
    $registryPaths = @(
        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
    )
    
    foreach ($regPath in $registryPaths) {
        if (Test-Path $regPath) {
            Get-ChildItem -Path $regPath -ErrorAction SilentlyContinue | ForEach-Object {
                $displayName = (Get-ItemProperty -Path $_.PSPath -Name DisplayName -ErrorAction SilentlyContinue).DisplayName
                if ($displayName -and $displayName -match 'Qt') {
                    $installLocation = (Get-ItemProperty -Path $_.PSPath -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
                    if ($installLocation -and (Test-Path $installLocation)) {
                        # Check if this is a Qt root directory, scan for MinGW installations
                        Get-ChildItem -Path $installLocation -Directory -ErrorAction SilentlyContinue | Where-Object {
                            $_.Name -match '^\d+\.\d+\.\d+$'
                        } | ForEach-Object {
                            Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue | Where-Object {
                                $_.Name -match 'mingw'
                            } | ForEach-Object {
                                $qmakePath = Join-Path $_.FullName "bin\qmake.exe"
                                if (Test-Path $qmakePath) {
                                    $found += $_.FullName
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    # Fallback: scan common paths if registry search found nothing
    if ($found.Count -eq 0) {
        $commonPaths = @(
            "C:\Qt",
            "D:\Qt",
            "E:\Qt",
            "F:\Qt",
            "$env:USERPROFILE\Qt",
            "$env:ProgramFiles\Qt",
            "${env:ProgramFiles(x86)}\Qt"
        )
        
        foreach ($basePath in $commonPaths) {
            if (Test-Path $basePath) {
                Get-ChildItem -Path $basePath -Directory -ErrorAction SilentlyContinue | Where-Object {
                    $_.Name -match '^\d+\.\d+\.\d+$'
                } | ForEach-Object {
                    Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue | Where-Object {
                        $_.Name -match 'mingw'
                    } | ForEach-Object {
                        $qmakePath = Join-Path $_.FullName "bin\qmake.exe"
                        if (Test-Path $qmakePath) {
                            $found += $_.FullName
                        }
                    }
                }
            }
        }
    }
    
    return $found | Select-Object -Unique
}

function Select-QtDirectory {
    Add-Type -AssemblyName System.Windows.Forms
    $folderBrowser = New-Object System.Windows.Forms.FolderBrowserDialog
    $folderBrowser.Description = "Select Qt MinGW installation directory (e.g., C:\Qt\6.10.2\mingw_64)"
    $folderBrowser.RootFolder = [System.Environment+SpecialFolder]::MyComputer
    $folderBrowser.ShowNewFolderButton = $false
    
    if ($folderBrowser.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        return $folderBrowser.SelectedPath
    }
    return $null
}

function Get-QtDirectory {
    # 1. Check if provided as parameter
    if ($QtDir) {
        Write-ColorOutput "Using Qt directory from parameter: $QtDir" Cyan
        return $QtDir
    }
    
    # 2. Check environment variable and try to find MinGW under it
    if ($env:QTDIR) {
        $qtdirPath = $env:QTDIR
        Write-ColorOutput "Found QTDIR environment variable: $qtdirPath" Cyan
        
        # Check if QTDIR directly points to a valid MinGW installation
        $qmake = Join-Path $qtdirPath "bin\qmake.exe"
        if ((Test-Path $qmake) -and ($qtdirPath.ToLower() -match 'mingw')) {
            Write-ColorOutput "QTDIR points to valid MinGW installation." Green
            return $qtdirPath
        }
        
        # QTDIR might point to Qt root, search for MinGW subdirectories
        Write-ColorOutput "QTDIR appears to be Qt root directory. Searching for MinGW installations..." Yellow
        $foundMinGW = @()
        
        Get-ChildItem -Path $qtdirPath -Directory -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -match '^\d+\.\d+\.\d+$'
        } | ForEach-Object {
            Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue | Where-Object {
                $_.Name -match 'mingw'
            } | ForEach-Object {
                $qmakePath = Join-Path $_.FullName "bin\qmake.exe"
                if (Test-Path $qmakePath) {
                    $foundMinGW += $_.FullName
                }
            }
        }
        
        if ($foundMinGW.Count -eq 1) {
            Write-ColorOutput "Found MinGW installation under QTDIR: $($foundMinGW[0])" Green
            $response = Read-Host "Use this installation? (Y/n)"
            if ($response -match '^(y|yes|)$' -or [string]::IsNullOrWhiteSpace($response)) {
                return $foundMinGW[0]
            }
        } elseif ($foundMinGW.Count -gt 1) {
            Write-ColorOutput "Found multiple MinGW installations under QTDIR:" Green
            for ($i = 0; $i -lt $foundMinGW.Count; $i++) {
                Write-Host "  [$($i+1)] $($foundMinGW[$i])"
            }
            
            do {
                $choice = Read-Host "Select Qt installation (1-$($foundMinGW.Count))"
                $choiceNum = 0
                $valid = [int]::TryParse($choice, [ref]$choiceNum) -and $choiceNum -ge 1 -and $choiceNum -le $foundMinGW.Count
            } until ($valid)
            
            return $foundMinGW[$choiceNum - 1]
        }
        
        Write-ColorOutput "No MinGW installations found under QTDIR." Yellow
    }
    
    # 3. Try to auto-detect
    Write-ColorOutput "Searching for Qt installations..." Yellow
    $detected = Find-QtInstallation
    
    if ($detected.Count -eq 1) {
        Write-ColorOutput "Found Qt installation: $($detected[0])" Green
        $response = Read-Host "Use this installation? (Y/n)"
        if ($response -match '^(y|yes|)$' -or [string]::IsNullOrWhiteSpace($response)) {
            return $detected[0]
        }
    } elseif ($detected.Count -gt 1) {
        Write-ColorOutput "Found multiple Qt installations:" Green
        for ($i = 0; $i -lt $detected.Count; $i++) {
            Write-Host "  [$($i+1)] $($detected[$i])"
        }
        Write-Host "  [0] Browse for directory manually"
        
        do {
            $choice = Read-Host "Select Qt installation (1-$($detected.Count), or 0 to browse)"
            $choiceNum = 0
            $valid = [int]::TryParse($choice, [ref]$choiceNum) -and $choiceNum -ge 0 -and $choiceNum -le $detected.Count
        } until ($valid)
        
        if ($choiceNum -gt 0) {
            return $detected[$choiceNum - 1]
        }
    }
    
    # 4. Manual input or browse
    Write-ColorOutput "No Qt MinGW installation auto-detected." Yellow
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  [1] Browse for Qt MinGW directory"
    Write-Host "  [2] Enter path manually"
    Write-Host "  [3] Exit"
    
    do {
        $choice = Read-Host "Select option (1-3)"
    } until ($choice -match '^[123]$')
    
    switch ($choice) {
        "1" {
            $dir = Select-QtDirectory
            if ($dir) { return $dir }
            Write-ColorOutput "No directory selected. Exiting." Red
            exit 1
        }
        "2" {
            $dir = Read-Host "Enter Qt MinGW directory path (e.g., C:\Qt\6.10.2\mingw_64)"
            if ($dir) { return $dir }
            Write-ColorOutput "No path entered. Exiting." Red
            exit 1
        }
        "3" {
            Write-ColorOutput "Exiting." Yellow
            exit 0
        }
    }
}

function Validate-QtDirectory {
    param([string]$Dir)
    
    $qmake = Join-Path $Dir "bin\qmake.exe"
    if (-not (Test-Path $qmake)) {
        Write-ColorOutput "ERROR: qmake.exe not found in $Dir\bin\" Red
        Write-ColorOutput "Please ensure you selected a valid Qt installation directory (e.g., C:\Qt\6.10.2\mingw_64)" Red
        return $false
    }
    
    # Validate it's a MinGW build
    $dirLower = $Dir.ToLower()
    if ($dirLower -notmatch 'mingw') {
        Write-ColorOutput "ERROR: Selected Qt installation is not MinGW-based." Red
        Write-ColorOutput "This project requires Qt with MinGW compiler (e.g., C:\Qt\6.10.2\mingw_64)" Red
        Write-ColorOutput "Selected path: $Dir" Red
        return $false
    }
    
    return $true
}

function Find-BuildTool {
    param([string]$QtDir, [string]$ToolName)
    
    $qtRoot = Split-Path (Split-Path $QtDir -Parent) -Parent
    $toolsDir = Join-Path $qtRoot "Tools"
    
    if (Test-Path $toolsDir) {
        # Search for tool in Qt Tools directory
        $toolExe = Get-ChildItem -Path $toolsDir -Recurse -Filter "$ToolName.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($toolExe) {
            return $toolExe.DirectoryName
        }
    }
    
    # Check system PATH
    $cmd = Get-Command "$ToolName.exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        return Split-Path $cmd.Source -Parent
    }
    
    return $null
}

function Find-MinGWDirectory {
    param([string]$QtDir)
    
    $qtRoot = Split-Path (Split-Path $QtDir -Parent) -Parent
    $toolsDir = Join-Path $qtRoot "Tools"
    
    if (Test-Path $toolsDir) {
        $mingwToolDirs = Get-ChildItem -Path $toolsDir -Directory -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -match 'mingw'
        }
        
        foreach ($dir in $mingwToolDirs) {
            $testBin = Join-Path $dir.FullName "bin\g++.exe"
            if (Test-Path $testBin) {
                return Join-Path $dir.FullName "bin"
            }
        }
    }
    
    return $null
}

# ──────────────────────────────────────────────────────────────────────────────
# Main Script
# ──────────────────────────────────────────────────────────────────────────────

Write-ColorOutput "`n========================================" Cyan
Write-ColorOutput "Reclass Build Script" Cyan
Write-ColorOutput "========================================`n" Cyan

# Get script directory and project root
$scriptDir = Split-Path -Parent $PSCommandPath
$projectRoot = Split-Path -Parent $scriptDir
$buildDir = Join-Path $projectRoot "build"

# Get Qt directory
$selectedQtDir = Get-QtDirectory
if (-not $selectedQtDir) {
    Write-ColorOutput "ERROR: No Qt directory provided." Red
    exit 1
}

# Validate Qt directory
if (-not (Validate-QtDirectory $selectedQtDir)) {
    exit 1
}

Write-ColorOutput "`nQt Directory: $selectedQtDir" Green
Write-ColorOutput "Build Type: $BuildType" Green

# Find build tools
Write-ColorOutput "`nSearching for build tools..." Cyan

$cmakeDir = Find-BuildTool $selectedQtDir "cmake"
$ninjaDir = Find-BuildTool $selectedQtDir "ninja"
$mingwDir = Find-MinGWDirectory $selectedQtDir

$missingTools = @()

if (-not $cmakeDir) {
    $missingTools += "CMake"
    Write-ColorOutput "  CMake: NOT FOUND" Red
} else {
    Write-ColorOutput "  CMake: $cmakeDir" Green
}

if (-not $ninjaDir) {
    $missingTools += "Ninja"
    Write-ColorOutput "  Ninja: NOT FOUND" Yellow
    Write-ColorOutput "         (Will try default CMake generator)" Gray
} else {
    Write-ColorOutput "  Ninja: $ninjaDir" Green
}

if (-not $mingwDir) {
    Write-ColorOutput "  MinGW: NOT FOUND (may cause build issues)" Yellow
} else {
    Write-ColorOutput "  MinGW: $mingwDir" Green
}

if ($missingTools.Count -gt 0 -and $missingTools -contains "CMake") {
    Write-ColorOutput "`nERROR: CMake is required but not found." Red
    Write-ColorOutput "Please install CMake or ensure it's in your PATH." Red
    Write-ColorOutput "You can download it from: https://cmake.org/download/" Red
    exit 1
}

# Build PATH environment
$pathDirs = @()
if ($cmakeDir) { $pathDirs += $cmakeDir }
if ($ninjaDir) { $pathDirs += $ninjaDir }
if ($mingwDir) { $pathDirs += $mingwDir }
$pathDirs += (Join-Path $selectedQtDir "bin")
$pathDirs += $env:Path

$env:Path = ($pathDirs -join ";")

Write-ColorOutput "`nEnvironment configured." Green

# Handle clean/rebuild
if ($Rebuild) {
    $Clean = $true
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-ColorOutput "`nCleaning build directory..." Yellow
    try {
        Remove-Item -Path $buildDir -Recurse -Force -ErrorAction Stop
        Write-ColorOutput "Build directory cleaned." Green
    } catch {
        Write-ColorOutput "ERROR: Failed to clean build directory: $_" Red
        exit 1
    }
}

# Create build directory
if (-not (Test-Path $buildDir)) {
    Write-ColorOutput "`nCreating build directory..." Cyan
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
}

# Change to build directory
Push-Location $buildDir

try {
    Write-ColorOutput "`nConfiguring CMake..." Cyan
    
    # Prepare CMake command
    $cmakeArgs = @(
        ".."
        "-DCMAKE_BUILD_TYPE=$BuildType"
        "-DCMAKE_PREFIX_PATH=$($selectedQtDir -replace '\\', '/')"
    )
    
    # Add generator if Ninja is available
    if ($ninjaDir) {
        $cmakeArgs = @("-G", "Ninja") + $cmakeArgs
        Write-ColorOutput "Using Ninja generator" Gray
    }
    
    Write-ColorOutput "Command: cmake $($cmakeArgs -join ' ')`n" Gray
    
    & cmake $cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-ColorOutput "`nERROR: CMake configuration failed with exit code $LASTEXITCODE" Red
        exit 1
    }
    Write-ColorOutput "`nCMake configuration completed successfully.`n" Green
    
    # Build
    Write-ColorOutput "Building Reclass..." Cyan
    
    $cores = (Get-CimInstance -ClassName Win32_Processor).NumberOfLogicalProcessors
    if (-not $cores -or $cores -lt 1) {
        $cores = 4
    }
    
    Write-ColorOutput "Command: cmake --build . --config $BuildType -j$cores`n" Gray
    
    & cmake --build . --config $BuildType "-j$cores"
    if ($LASTEXITCODE -ne 0) {
        Write-ColorOutput "`nERROR: Build failed with exit code $LASTEXITCODE" Red
        exit 1
    }
    Write-ColorOutput "`nBuild completed successfully!`n" Green
    
    # Find executable
    Write-ColorOutput "Locating executable..." Cyan
    $exePaths = @(
        (Join-Path $buildDir "Reclass.exe"),
        (Join-Path $buildDir "$BuildType\Reclass.exe")
    )
    
    $exePath = $null
    foreach ($path in $exePaths) {
        if (Test-Path $path) {
            $exePath = $path
            break
        }
    }
    
    if ($exePath) {
        $fileInfo = Get-Item $exePath
        $sizeMB = [math]::Round($fileInfo.Length / 1MB, 2)
        Write-ColorOutput "Executable: $exePath" Green
        Write-ColorOutput "Size: $sizeMB MB" Gray
        
        # Run windeployqt to copy Qt dependencies
        Write-ColorOutput "`nRunning windeployqt..." Cyan
        $windeployqt = Join-Path $selectedQtDir "bin\windeployqt.exe"
        
        if (Test-Path $windeployqt) {
            $exeDir = Split-Path $exePath -Parent
            Write-ColorOutput "Command: `"$windeployqt`" `"$exePath`"`n" Gray
            
            & $windeployqt $exePath
            if ($LASTEXITCODE -eq 0) {
                Write-ColorOutput "windeployqt completed successfully." Green
                
                # Count deployed files
                $deployedFiles = Get-ChildItem -Path $exeDir -Recurse -File | Where-Object {
                    $_.Name -ne "Reclass.exe" -and $_.Extension -match '\.(dll|qm)$'
                }
                if ($deployedFiles) {
                    Write-ColorOutput "Deployed $($deployedFiles.Count) Qt dependency files." Gray
                }
            } else {
                Write-ColorOutput "WARNING: windeployqt failed with exit code $LASTEXITCODE" Yellow
                Write-ColorOutput "Application may not run without Qt DLLs in PATH" Yellow
            }
        } else {
            Write-ColorOutput "WARNING: windeployqt.exe not found at $windeployqt" Yellow
            Write-ColorOutput "Application may not run without Qt DLLs in PATH" Yellow
        }
    } else {
        Write-ColorOutput "WARNING: Could not locate Reclass.exe" Yellow
    }
    
} catch {
    Write-ColorOutput "`nERROR: Build failed with exception: $_" Red
    exit 1
} finally {
    Pop-Location
}

Write-ColorOutput "`n========================================" Cyan
Write-ColorOutput "Build completed successfully!" Cyan
Write-ColorOutput "========================================`n" Cyan

if ($exePath) {
    Write-ColorOutput "Run the application with:" White
    Write-ColorOutput "  .\build\Reclass.exe`n" Cyan
}
