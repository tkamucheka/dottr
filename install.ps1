# Installs dottr on Windows: the programs dottr, dottr-format, and dottr-lsp,
# from the release archive, into %LOCALAPPDATA%\Programs\dottr\bin, which is
# added to the user's PATH.
#
#   irm https://raw.githubusercontent.com/tkamucheka/dottr/main/install.ps1 | iex
#
# Options are environment variables, since a script run with iex takes no
# arguments:
#
#   DOTTR_VERSION   release to install (default: the latest)
#   DOTTR_ARCHIVE   install a downloaded release archive instead
#   DOTTR_PREFIX    installation directory (default:
#                   %LOCALAPPDATA%\Programs\dottr); programs go to PREFIX\bin
#   DOTTR_NO_PATH   if set, do not add PREFIX\bin to the user's PATH
#   DOTTR_RELEASES  base URL of the releases
#
# The script runs in Windows PowerShell 5.1 and PowerShell 7. It is a single
# function that is called on its last line, so that a partially downloaded
# script does nothing.

function Install-Dottr {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'

    function Say([string] $Message) {
        Write-Host "dottr-install: $Message"
    }

    function Fail([string] $Message) {
        throw "dottr-install: error: $Message"
    }

    $programs = 'dottr', 'dottr-format', 'dottr-lsp'
    $asset = 'dottr-windows-x86_64.zip'
    $releases = if ($env:DOTTR_RELEASES) { $env:DOTTR_RELEASES } else {
        'https://github.com/tkamucheka/dottr/releases'
    }
    $version = if ($env:DOTTR_VERSION) { $env:DOTTR_VERSION } else { 'latest' }
    $prefix = if ($env:DOTTR_PREFIX) { $env:DOTTR_PREFIX } else {
        if (-not $env:LOCALAPPDATA) { Fail 'LOCALAPPDATA is not set' }
        Join-Path $env:LOCALAPPDATA 'Programs\dottr'
    }
    $bin = Join-Path $prefix 'bin'

    # Windows PowerShell 5.1 does not enable TLS 1.2 by default.
    if ($PSVersionTable.PSVersion.Major -lt 6) {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor
            [Net.SecurityProtocolType]::Tls12
    }

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("dottr-install-" +
        [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        # 1. Obtain the archive.
        if ($env:DOTTR_ARCHIVE) {
            if (-not (Test-Path -PathType Leaf $env:DOTTR_ARCHIVE)) {
                Fail "$env:DOTTR_ARCHIVE does not exist"
            }
            $archive = $env:DOTTR_ARCHIVE
            Say "installing $archive"
        } else {
            $base = if ($version -eq 'latest') { "$releases/latest/download" }
                    else { "$releases/download/$version" }
            $archive = Join-Path $tmp $asset
            $sums = Join-Path $tmp 'SHA256SUMS'
            Say "downloading $base/$asset"
            try {
                Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset" -OutFile $archive
            } catch {
                Fail "cannot download $base/$asset; is there a release $version?"
            }
            try {
                Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums
            } catch {
                Fail 'the release has no SHA256SUMS'
            }
            $expected = $null
            foreach ($line in Get-Content $sums) {
                $fields = $line -split '\s+', 2
                if ($fields.Count -eq 2 -and $fields[1].TrimStart('*') -eq $asset) {
                    $expected = $fields[0]
                }
            }
            if (-not $expected) { Fail "SHA256SUMS does not list $asset" }
            $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash
            if ($actual -ne $expected.ToUpperInvariant()) {
                Fail "the checksum of $asset does not match SHA256SUMS"
            }
        }

        # 2. Extract and check the programs before installing any.
        $unpacked = Join-Path $tmp 'archive'
        Expand-Archive -Path $archive -DestinationPath $unpacked
        $found = @{}
        foreach ($program in $programs) {
            $file = Get-ChildItem -Path $unpacked -Recurse -Filter "$program.exe" |
                Where-Object { $_.Directory.Name -eq 'bin' } |
                Select-Object -First 1
            if (-not $file) { Fail "the archive does not contain bin\$program.exe" }
            & $file.FullName --help | Out-Null
            if ($LASTEXITCODE -ne 0) { Fail "$program does not run on this system" }
            $found[$program] = $file.FullName
        }

        # 3. Install. A running program cannot be replaced on Windows, but it
        # can be renamed, so the old program is moved aside first.
        New-Item -ItemType Directory -Force -Path $bin | Out-Null
        foreach ($program in $programs) {
            $target = Join-Path $bin "$program.exe"
            $old = "$target.old"
            Remove-Item -Force -ErrorAction SilentlyContinue $old
            if (Test-Path $target) { Move-Item -Force $target $old }
            Copy-Item $found[$program] $target
            Remove-Item -Force -ErrorAction SilentlyContinue $old
            Say "installed $target"
        }

        # 4. Add the directory to the user's PATH.
        if ($env:DOTTR_NO_PATH) { return }
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        $entries = @($userPath -split ';' | Where-Object { $_ })
        if ($entries -notcontains $bin) {
            [Environment]::SetEnvironmentVariable('Path',
                (($entries + $bin) -join ';'), 'User')
            Say "added $bin to the user's PATH; open a new terminal to use it"
        }
        if (@($env:Path -split ';') -notcontains $bin) {
            $env:Path = "$env:Path;$bin"
        }
    } finally {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tmp
    }
}

Install-Dottr
