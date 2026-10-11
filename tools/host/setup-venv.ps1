[CmdletBinding()]
param(
    [string]$VenvDir,
    [string]$Python = "python"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$requirementsFile = Join-Path $PSScriptRoot "requirements-coverage.txt"
$gcovrVersion = "8.6"

if ([string]::IsNullOrWhiteSpace($VenvDir)) {
    $VenvDir = Join-Path $repoRoot "build/coverage-tools"
} elseif ($VenvDir -eq "~") {
    $VenvDir = $HOME
} elseif ($VenvDir.StartsWith("~/") -or $VenvDir.StartsWith("~\")) {
    $VenvDir = Join-Path $HOME $VenvDir.Substring(2)
}
if (-not [System.IO.Path]::IsPathRooted($VenvDir)) {
    $VenvDir = Join-Path (Get-Location).Path $VenvDir
}
$VenvDir = [System.IO.Path]::GetFullPath($VenvDir)

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable exited with status $LASTEXITCODE"
    }
}

try {
    if (-not (Test-Path $requirementsFile)) {
        throw "Coverage requirements file not found: $requirementsFile"
    }
    Invoke-Checked -Executable $Python -Arguments @(
        "-c", "import sys; raise SystemExit(sys.version_info < (3, 10))"
    )

    $venvPython = Join-Path $VenvDir "Scripts/python.exe"
    $gcovrExecutable = Join-Path $VenvDir "Scripts/gcovr.exe"
    $gcovrVersionLine = ""
    if (Test-Path $gcovrExecutable) {
        $gcovrOutput = & $gcovrExecutable --version
        if ($LASTEXITCODE -eq 0) {
            $gcovrVersionLine = $gcovrOutput | Select-Object -First 1
        }
    }

    if ($gcovrVersionLine -ne "gcovr $gcovrVersion") {
        $uvCommand = Get-Command uv -ErrorAction SilentlyContinue
        if ($null -ne $uvCommand) {
            Invoke-Checked -Executable $uvCommand.Source -Arguments @(
                "venv", "--clear", "--python", $Python, $VenvDir
            )
            Invoke-Checked -Executable $uvCommand.Source -Arguments @(
                "pip", "install", "--python", $venvPython, "-r", $requirementsFile
            )
        } else {
            Invoke-Checked -Executable $Python -Arguments @("-m", "venv", "--clear", $VenvDir)
            Invoke-Checked -Executable $venvPython -Arguments @(
                "-m", "pip", "install", "--disable-pip-version-check", "--no-input", "-r", $requirementsFile
            )
        }
    } else {
        Invoke-Checked -Executable $venvPython -Arguments @(
            "-m", "pip", "install", "--disable-pip-version-check", "--no-input", "-r", $requirementsFile
        )
    }

    $gcovrOutput = & $gcovrExecutable --version
    if ($LASTEXITCODE -ne 0 -or ($gcovrOutput | Select-Object -First 1) -ne "gcovr $gcovrVersion") {
        throw "gcovr $gcovrVersion was not installed in $VenvDir"
    }
} catch {
    Write-Error $_
    exit 1
}

$powershellActivate = Join-Path $VenvDir "Scripts/Activate.ps1"
$commandPromptActivate = Join-Path $VenvDir "Scripts/activate.bat"
Write-Host "Environment ready: $VenvDir"
Write-Host "To activate in this PowerShell session, run: . `"$powershellActivate`""
Write-Host "Command Prompt: `"$commandPromptActivate`""
Write-Host "Check gcovr with: `"$gcovrExecutable`" --version"