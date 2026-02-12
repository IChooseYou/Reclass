# PowerShell script to build QScintilla static library for Reclass
# This script checks for Qt installation, prompts if missing, and builds QScintilla

#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory=$false)]
    [string]$QtDir,
    
    [Parameter(Mandatory=$false)]
    [switch]$Clean
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
            "F:\Qt"
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

function Find-MakeCommand {
    param([string]$QtDir)
    
    # Look for mingw32-make in Qt Tools directory
    $qtRoot = Split-Path (Split-Path $QtDir -Parent) -Parent
    $toolsDir = Join-Path $qtRoot "Tools"
    
    if (Test-Path $toolsDir) {
        $mingwMake = Get-ChildItem -Path $toolsDir -Recurse -Filter "mingw32-make.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($mingwMake) {
            return $mingwMake.FullName
        }
    }
    
    # Check system PATH
    $makeCmd = Get-Command mingw32-make.exe -ErrorAction SilentlyContinue
    if ($makeCmd) {
        return $makeCmd.Source
    }
    
    Write-ColorOutput "WARNING: mingw32-make.exe not found. Please ensure MinGW is in PATH." Yellow
    return "mingw32-make.exe"
}

# ──────────────────────────────────────────────────────────────────────────────
# Main Script
# ──────────────────────────────────────────────────────────────────────────────

Write-ColorOutput "`n========================================" Cyan
Write-ColorOutput "QScintilla Build Script for Reclass" Cyan
Write-ColorOutput "========================================`n" Cyan

# Get script directory and project root
$scriptDir = Split-Path -Parent $PSCommandPath
$projectRoot = Split-Path -Parent $scriptDir
$qsciSrcDir = Join-Path $projectRoot "third_party\qscintilla\src"

# Check if QScintilla source exists
if (-not (Test-Path $qsciSrcDir)) {
    Write-ColorOutput "ERROR: QScintilla source not found at $qsciSrcDir" Red
    Write-ColorOutput "Please extract QScintilla source to third_party/qscintilla/" Red
    exit 1
}

# Verify qscintilla.pro exists
$proFile = Join-Path $qsciSrcDir "qscintilla.pro"
if (-not (Test-Path $proFile)) {
    Write-ColorOutput "ERROR: qscintilla.pro not found at $proFile" Red
    exit 1
}

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

# Find make command
$makeCmd = Find-MakeCommand $selectedQtDir
Write-ColorOutput "Make Command: $makeCmd" Green

# Find MinGW bin directory (contains g++, cc1plus, etc.)
$mingwBinDir = $null
$qtRoot = Split-Path (Split-Path $selectedQtDir -Parent) -Parent
$toolsDir = Join-Path $qtRoot "Tools"

if (Test-Path $toolsDir) {
    # Look for MinGW tools directory
    $mingwToolDirs = Get-ChildItem -Path $toolsDir -Directory -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -match 'mingw'
    }
    
    foreach ($dir in $mingwToolDirs) {
        $testBin = Join-Path $dir.FullName "bin\g++.exe"
        if (Test-Path $testBin) {
            $mingwBinDir = Join-Path $dir.FullName "bin"
            break
        }
    }
}

# Set up environment - add both Qt bin and MinGW bin to PATH
$qmakePath = Join-Path $selectedQtDir "bin\qmake.exe"
if ($mingwBinDir) {
    Write-ColorOutput "MinGW Directory: $mingwBinDir" Green
    $env:Path = "$mingwBinDir;$selectedQtDir\bin;$env:Path"
} else {
    Write-ColorOutput "WARNING: MinGW tools directory not found. Build may fail." Yellow
    Write-ColorOutput "Ensure MinGW bin directory (containing g++.exe, cc1plus.exe) is in PATH" Yellow
    $env:Path = "$selectedQtDir\bin;$env:Path"
}

# Clean if requested
if ($Clean) {
    Write-ColorOutput "`nCleaning previous build..." Yellow
    Push-Location $qsciSrcDir
    try {
        if (Test-Path "Makefile") {
            & $makeCmd clean 2>&1 | Out-Null
        }
        Remove-Item -Path "Makefile*", "release", "debug", "*.o", "*.obj" -Recurse -Force -ErrorAction SilentlyContinue
        Write-ColorOutput "Clean complete." Green
    } finally {
        Pop-Location
    }
}

# Change to QScintilla source directory
Write-ColorOutput "`nChanging to QScintilla source directory..." Cyan
Push-Location $qsciSrcDir

try {
    Write-ColorOutput "Current directory: $PWD`n" Gray
    
    # Run qmake
    Write-ColorOutput "Running qmake..." Cyan
    Write-ColorOutput "Command: `"$qmakePath`" qscintilla.pro `"CONFIG+=staticlib`"`n" Gray
    
    & $qmakePath qscintilla.pro "CONFIG+=staticlib"
    if ($LASTEXITCODE -ne 0) {
        Write-ColorOutput "`nERROR: qmake failed with exit code $LASTEXITCODE" Red
        exit 1
    }
    Write-ColorOutput "qmake completed successfully.`n" Green
    
    # Run make
    Write-ColorOutput "Running $makeCmd..." Cyan
    $cores = (Get-CimInstance -ClassName Win32_Processor).NumberOfLogicalProcessors
    if (-not $cores -or $cores -lt 1) {
        $cores = 4  # Default fallback
    }
    Write-ColorOutput "Command: `"$makeCmd`" -j$cores`n" Gray
    
    & $makeCmd "-j$cores"
    if ($LASTEXITCODE -ne 0) {
        Write-ColorOutput "`nERROR: make failed with exit code $LASTEXITCODE" Red
        exit 1
    }
    Write-ColorOutput "`nmake completed successfully.`n" Green
    
    # Check for output
    Write-ColorOutput "Checking for generated library files..." Cyan
    $foundLibs = @()
    
    $patterns = @("*.a", "*.lib")
    $searchDirs = @(".", "release", "debug")
    
    foreach ($dir in $searchDirs) {
        $fullPath = Join-Path $PWD $dir
        if (Test-Path $fullPath) {
            foreach ($pattern in $patterns) {
                $libs = Get-ChildItem -Path $fullPath -Filter $pattern -ErrorAction SilentlyContinue
                if ($libs) {
                    $foundLibs += $libs | ForEach-Object { 
                        [PSCustomObject]@{
                            Name = $_.Name
                            Path = $_.FullName
                            Size = $_.Length
                        }
                    }
                }
            }
        }
    }
    
    if ($foundLibs.Count -gt 0) {
        Write-ColorOutput "`nBuild successful! Generated libraries:" Green
        foreach ($lib in $foundLibs) {
            $sizeMB = [math]::Round($lib.Size / 1MB, 2)
            Write-Host "  - $($lib.Name) ($sizeMB MB)" -ForegroundColor Green
            Write-Host "    Path: $($lib.Path)" -ForegroundColor Gray
        }
        Write-ColorOutput "`nYou can now build Reclass with CMake." Green
    } else {
        Write-ColorOutput "`nWARNING: Build completed but no library files found." Yellow
        Write-ColorOutput "Expected files: qscintilla2_qt6.a or qscintilla2_qt6.lib" Yellow
    }
    
} catch {
    Write-ColorOutput "`nERROR: Build failed with exception: $_" Red
    exit 1
} finally {
    Pop-Location
}

Write-ColorOutput "`n========================================" Cyan
Write-ColorOutput "Build script completed successfully" Cyan
Write-ColorOutput "========================================`n" Cyan
