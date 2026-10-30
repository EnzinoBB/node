# Guided set-up of the node in this folder (Windows): the same questions, the same config.ini and the
# same contract executor choice as setup.sh. Started by setup.bat; "setup.bat --defaults" takes every
# default without asking.
param([switch]$Defaults)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

$node = '.\node.exe'
$config = 'config.ini'
if (-not (Test-Path $node) -or -not (Test-Path $config)) {
    Write-Host 'Run setup.bat from the csnode folder of the package (node.exe and config.ini are not here).'
    exit 1
}

function Ask([string]$Question, [string]$Default, [string[]]$Choices) {
    while ($true) {
        if ($Defaults) { Write-Host "$Question [$Default]: $Default"; return $Default }
        if ([Console]::IsInputRedirected) {
            Write-Host -NoNewline "${Question} [$Default]: "
            $reply = [Console]::In.ReadLine()
            if ($null -eq $reply) { $reply = '' }
            Write-Host $reply
        } else {
            $reply = Read-Host "$Question [$Default]"
        }
        $reply = $reply.Trim().ToLower()
        if ($reply -eq '') { return $Default }
        foreach ($c in $Choices) { if ($c.StartsWith($reply)) { return $c } }
        Write-Host "  Please answer one of: $($Choices -join ' ')"
    }
}

function Existing-Backend {
    if (Test-Path 'db\.backend') { return (Get-Content 'db\.backend' -Raw).Trim() }
    if ((Test-Path 'db\blockchain.db') -or (Test-Path 'db\sequence.db')) { return 'berkeleydb' }
    if (Test-Path 'db\lmdb\data.mdb') { return 'lmdb' }
    if (Test-Path 'db\CURRENT') { return 'rocksdb' }
    return ''
}

$lines = @(Get-Content -LiteralPath $config)

function Current([string]$Section, [string]$Key) {
    $in = $false; $value = ''
    foreach ($l in $lines) {
        if ($l -match '^\s*\[') { $in = ($l -match "^\s*\[$Section\]"); continue }
        if ($in -and $l -match "^\s*$Key\s*=\s*(.*?)\s*$") { $value = $Matches[1] }
    }
    return $value
}

# --- contract executor ---------------------------------------------------------------------------
# The node runs smart contracts with contract-executor.jar (it loads sc-api-support.jar from the same
# folder) and starts it only if the build the jar reports (its git commit count) is within
# [executor_commit_min, executor_commit_max] of [api]; otherwise it stops the executor and runs no
# contract. The official release is build 1518; community builds add fixes on top and count higher.
$officialBuild = 1518
$officialUrl = 'https://github.com/CREDITSCOM/node/releases/download/mainnet-4.2.532.0/Ubuntu20_Mainnet-4.2.532.0.tar.gz'
$officialShaExecutor = '592510d1586818348c65bea9b443e7f58aa6947113e43aa8906af8426aeef028'
$officialShaSupport = '395d9a108a66fe1d92c1d2096a9d11f4c7662b51b47af792db0ff4cc91df9807'
$communityUrl = 'https://github.com/EnzinoBB/contract-executor/releases/latest/download'
Add-Type -AssemblyName System.IO.Compression.FileSystem

# the build (git commit count) an executor jar reports, 0 when it cannot be read
function Jar-Build([string]$Jar) {
    if (-not (Test-Path -LiteralPath $Jar)) { return 0 }
    try {
        $zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Jar).Path)
        try {
            $entry = $zip.GetEntry('git.properties')
            if ($null -eq $entry) { return 0 }
            $reader = New-Object System.IO.StreamReader($entry.Open())
            try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $zip.Dispose() }
        if ($text -match '(?m)^git\.total\.commit\.count=(\d+)') { return [int]$Matches[1] }
    } catch { }
    return 0
}

function Describe-Build([int]$Build) {
    if ($Build -eq 0) { return 'none or unreadable' }
    if ($Build -eq $officialBuild) { return "build $Build (official)" }
    if ($Build -gt $officialBuild) { return "build $Build (community)" }
    return "build $Build"
}

function Sha256([string]$Path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLower() }

# downloads the jars of the chosen executor into $Dir; prints the reason and returns $false when it cannot
function Download-Executor([string]$Kind, [string]$Dir) {
    if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) { Write-Host '   curl.exe is not available.'; return $false }
    if ($Kind -eq 'community') {
        foreach ($f in @('contract-executor.jar', 'sc-api-support.jar', 'SHA256SUMS')) {
            & curl.exe -fsSL -o (Join-Path $Dir $f) "$communityUrl/$f"
            if ($LASTEXITCODE -ne 0) { Write-Host '   The download from GitHub failed.'; return $false }
        }
        $sums = Get-Content -LiteralPath (Join-Path $Dir 'SHA256SUMS')
        foreach ($f in @('contract-executor.jar', 'sc-api-support.jar')) {
            $sha = Sha256 (Join-Path $Dir $f)
            if (-not ($sums | Where-Object { $_ -match "^$sha\s+\*?$([regex]::Escape($f))$" })) {
                Write-Host "   $f does not match the SHA256SUMS of the release."; return $false
            }
        }
    } else {
        Write-Host '   The jars come from the official release package (about 350 MB to download, only they are kept).'
        $archive = Join-Path $Dir 'official.tar.gz'
        & curl.exe -fsSL -o $archive $officialUrl
        if ($LASTEXITCODE -ne 0) { Write-Host '   The download of the official release failed.'; return $false }
        & tar.exe -xzf $archive -C $Dir csnode/contract-executor.jar csnode/sc-api-support.jar
        if ($LASTEXITCODE -ne 0) { Write-Host '   The official release could not be extracted.'; return $false }
        Remove-Item -LiteralPath $archive
        $extracted = Join-Path $Dir 'csnode'
        Move-Item -LiteralPath (Join-Path $extracted 'contract-executor.jar'), (Join-Path $extracted 'sc-api-support.jar') -Destination $Dir
        if ((Sha256 (Join-Path $Dir 'contract-executor.jar')) -ne $officialShaExecutor -or
            (Sha256 (Join-Path $Dir 'sc-api-support.jar')) -ne $officialShaSupport) {
            Write-Host '   The jars of the official release do not have the expected checksums.'; return $false
        }
    }
    return $true
}

# key=value in [api] of the lines, added at the end of the section when missing
function Set-ApiKey([System.Collections.Generic.List[string]]$Lines, [string]$Key, [string]$Value) {
    $in = $false; $end = -1
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match '^\s*\[') {
            if ($in -and $end -lt 0) { $end = $i }
            $in = ($Lines[$i] -match '^\s*\[api\]')
            continue
        }
        if ($in -and $Lines[$i] -match "^\s*$Key\s*=") { $Lines[$i] = "$Key=$Value"; return }
    }
    if ($in) { $Lines.Add("$Key=$Value") } elseif ($end -ge 0) { $Lines.Insert($end, "$Key=$Value") }
}

$version = if (Test-Path 'VERSION') { (Get-Content 'VERSION' -Raw).Trim() } else { 'unknown version' }
Write-Host "Credits node set-up ($version)"
Write-Host 'Each question shows the default in brackets: press Enter to keep it.'
Write-Host ''

$have = Existing-Backend
if ($have) { Write-Host "This folder already holds a $have database: it is kept."; Write-Host '' }

$curBackend = Current 'storage' 'db_backend'
if (-not $curBackend) { $curBackend = Current 'storage' 'block_db' }
$curPrune = Current 'storage' 'prune_keep_blocks'
$curServe = Current 'snapshot' 'serve'

# 1. pruned or full
$storageDefault = 'pruned'
if (($curBackend + $curPrune) -and (-not $curPrune -or $curPrune -eq '0')) { $storageDefault = 'full' }
if ($have -eq 'berkeleydb') { $storageDefault = 'full' }
Write-Host '1. Storage'
Write-Host '   pruned: keeps only recent blocks (a few GB), starts in minutes from a snapshot. Enough to run a node.'
Write-Host '   full:   keeps every block since the start of the chain (tens of GB), for history and explorers.'
$storage = Ask '   Pruned or full node? (pruned/full)' $storageDefault @('pruned', 'full')
if ($storage -eq 'full' -and $have -and $curPrune -and $curPrune -ne '0') {
    Write-Host '   This folder holds a pruned database: its removed blocks are gone. To get a full node,'
    Write-Host '   move db, caches and qs away and run the set-up again on the empty folder.'
}
Write-Host ''

# 2. database
Write-Host '2. Database'
if ($storage -eq 'pruned') {
    $backendDefault = if ($curBackend -eq 'rocksdb' -or $have -eq 'rocksdb') { 'rocksdb' } else { 'lmdb' }
    Write-Host '   lmdb:    the usual choice for pruned nodes.'
    Write-Host '   rocksdb: compressed, smaller on disk, a little more CPU.'
    $backend = Ask '   Database? (lmdb/rocksdb)' $backendDefault @('lmdb', 'rocksdb')
} else {
    $backendDefault = 'berkeleydb'
    if (($curBackend -eq 'lmdb' -or $curBackend -eq 'rocksdb') -and (-not $curPrune -or $curPrune -eq '0')) { $backendDefault = $curBackend }
    if ($have) { $backendDefault = $have }
    Write-Host "   berkeleydb: the official node's format (the official node can open this folder too)."
    Write-Host '   rocksdb:    the smallest full node (about a third of berkeleydb).'
    Write-Host '   lmdb:       fast, larger on disk.'
    $backend = Ask '   Database? (berkeleydb/rocksdb/lmdb)' $backendDefault @('berkeleydb', 'rocksdb', 'lmdb')
}
Write-Host ''

$convert = $false
if ($have -and $have -ne $backend) {
    Write-Host "   The database in this folder is $have, you chose $backend."
    $a = Ask "   Convert it to $backend at the next start (it can take hours)? (yes/no)" 'yes' @('yes', 'no')
    if ($a -eq 'yes') { $convert = $true } else { $backend = $have; Write-Host "   Keeping $have." }
    Write-Host ''
}

# 3. how the first start gets the chain (only for an empty folder)
$first = 'keep'
if (-not $have) {
    Write-Host '3. First start'
    if ($backend -eq 'berkeleydb') {
        Write-Host '   A berkeleydb node syncs from block 0 (about 35 hours on mainnet).'
        $first = 'sync'
    } else {
        Write-Host '   github:  download the snapshot published on GitHub now, check it and restore it (minutes).'
        Write-Host '   network: the node fetches a snapshot from other nodes that share it; it needs at least two'
        Write-Host '            of them, and few nodes share yet: without them it syncs from block 0 after 10 minutes.'
        Write-Host '   sync:    sync from block 0 by itself (about 35 hours on mainnet).'
        $firstDefault = 'github'
        if ($storage -eq 'full') {
            Write-Host '   A full node started from a snapshot holds the blocks from the snapshot on only.'
            $firstDefault = 'sync'
        }
        $first = Ask '   How should the node get the chain? (github/network/sync)' $firstDefault @('github', 'network', 'sync')
    }
    Write-Host ''
}

# 4. sharing snapshots
$serveDefault = if ($curServe -eq 'off') { 'no' } else { 'yes' }
Write-Host '4. Sharing'
Write-Host '   The node can share snapshots of the chain with new nodes, so that they start in minutes.'
Write-Host '   It uses at most 2 MB/s of upload, only while the node is synced, and about 200 MB of disk.'
$serve = Ask '   Share snapshots with other nodes? (yes/no)' $serveDefault @('yes', 'no')
Write-Host ''

# 5. the contract executor (when config.ini runs one)
$executorChoice = 'none'
$executorAction = 'keep'
if (Current 'api' 'executor_command') {
    $haveBuild = Jar-Build 'contract-executor.jar'
    $supportBuild = Jar-Build 'sc-api-support.jar'
    $curMin = Current 'api' 'executor_commit_min'
    $curMax = Current 'api' 'executor_commit_max'
    Write-Host '5. Contract executor'
    Write-Host '   The node runs smart contracts with contract-executor.jar and sc-api-support.jar, and starts'
    Write-Host '   the executor only if its build is within executor_commit_min..executor_commit_max of config.ini.'
    Write-Host ("   In this folder: $(Describe-Build $haveBuild); config.ini accepts " + $(if ($curMin) { $curMin } else { '1506' }) + '..' + $(if ($curMax) { $curMax } else { 'any' }) + '.')
    if ($haveBuild -and $supportBuild -and $haveBuild -ne $supportBuild) {
        Write-Host "   sc-api-support.jar is $(Describe-Build $supportBuild): the two jars come from different packages."
    }
    Write-Host '   community: the latest community build, the official one with fixes (code of a contract that never'
    Write-Host '              returns is stopped, tokens of every standard are reported to the API). Contracts give'
    Write-Host '              the same results as with the official one.'
    Write-Host "   official:  build $officialBuild of the official release."
    # unattended (--defaults), nothing is downloaded: the jars in the folder are kept
    $downloadDefault = if ($Defaults) { 'no' } else { 'yes' }
    $executorDefault = if ($haveBuild -gt $officialBuild) { 'community' } else { 'official' }
    $executorChoice = Ask '   Which executor? (community/official)' $executorDefault @('community', 'official')
    if ($executorChoice -eq 'community' -and $haveBuild -le $officialBuild) {
        Write-Host '   The jars in this folder are not a community build (copying only node.exe keeps the old ones).'
        $a = Ask '   Download the latest community executor from GitHub now (about 25 MB)? (yes/no)' $downloadDefault @('yes', 'no')
        $executorAction = if ($a -eq 'yes') { 'download' } else { 'manual' }
    } elseif ($executorChoice -eq 'official' -and $haveBuild -ne $officialBuild) {
        if ((Test-Path 'contract-executor.jar.official') -and (Test-Path 'sc-api-support.jar.official')) {
            Write-Host '   The official jars set aside by an earlier set-up are here: they are put back.'
            $executorAction = 'restore'
        } else {
            $a = Ask '   Download the official executor now (from the official release, about 350 MB)? (yes/no)' $downloadDefault @('yes', 'no')
            $executorAction = if ($a -eq 'yes') { 'download' } else { 'manual' }
        }
    }
    Write-Host ''
}

# 6. start now
$start = Ask '6. Start the node when the set-up is done? (yes/no)' 'yes' @('yes', 'no')
Write-Host ''

Write-Host 'Summary'
Write-Host "  storage:      $storage"
Write-Host ("  database:     $backend" + $(if ($convert) { " (converted from $have at the next start)" } else { '' }))
switch ($first) {
    'keep'    { Write-Host '  first start:  the database in this folder is kept' }
    'network' { Write-Host '  first start:  snapshot from other nodes' }
    'github'  { Write-Host '  first start:  snapshot downloaded from GitHub now' }
    'sync'    { Write-Host '  first start:  sync from block 0' }
}
Write-Host "  share:        $serve"
if ($executorChoice -ne 'none') {
    switch ($executorAction) {
        'keep'     { Write-Host "  executor:     $executorChoice, the jars in this folder are kept" }
        'download' { Write-Host "  executor:     $executorChoice, downloaded now (the jars in this folder are set aside)" }
        'restore'  { Write-Host '  executor:     official, the jars set aside before are put back' }
        'manual'   { Write-Host "  executor:     $executorChoice, to copy by hand (see below)" }
    }
}
Write-Host "  start now:    $start"
if (-not $Defaults) {
    if ((Ask 'Write config.ini? (yes/no)' 'yes' @('yes', 'no')) -ne 'yes') { Write-Host 'Nothing changed.'; exit 0 }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = "$config.bak-$stamp"
Copy-Item -LiteralPath $config -Destination $backup

$fetch = if ($first -eq 'network') { 'auto' } else { 'off' }
$serveValue = if ($serve -eq 'yes') { 'on' } else { 'off' }

# config.ini without its [snapshot] and [storage] sections, then the new ones ([snapshot] first)
$out = New-Object System.Collections.Generic.List[string]
$skip = $false; $blank = $false
foreach ($l in $lines) {
    if ($l -match '^\s*\[') { $skip = ($l -match '^\s*\[(snapshot|storage)\]') }
    if ($skip -or $l -match '^; written by setup\.(sh|bat) on ') { continue }
    if ($l.Trim() -eq '') { if ($blank) { continue }; $blank = $true } else { $blank = $false }
    $out.Add($l)
}
$out.Add('')
$out.Add("; written by setup.bat on $(Get-Date -Format 'yyyy-MM-dd HH:mm'), the previous file is $backup")
$out.Add('[snapshot]')
$out.Add("serve=$serveValue")
$out.Add("fetch=$fetch")
$out.Add('')
$out.Add('[storage]')
$out.Add("db_backend=$backend")
if ($storage -eq 'pruned') {
    $out.Add('checkpoint_every=100000')
    $out.Add('checkpoint_keep=2')
    $out.Add('prune_keep_blocks=1000')
}

# the executor: put the chosen jars in place, then make config.ini accept their build
$executorNotes = New-Object System.Collections.Generic.List[string]
if ($executorChoice -ne 'none') {
    # keeps the jars in this folder aside: as .official when they are the official ones (a later
    # set-up puts them back without a download), as .bak-<date> otherwise
    function Set-Aside {
        $suffix = if ((Jar-Build 'contract-executor.jar') -eq $officialBuild) { '.official' } else { ".bak-$stamp" }
        foreach ($f in @('contract-executor.jar', 'sc-api-support.jar')) {
            if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f$suffix" -Force }
        }
        $executorNotes.Add("The previous executor jars are kept as contract-executor.jar$suffix and sc-api-support.jar$suffix.")
    }
    if ($executorAction -eq 'download') {
        Write-Host ''
        Write-Host "Downloading the $executorChoice executor..."
        $dl = Join-Path ([System.IO.Path]::GetTempPath()) ("credits-executor-" + [guid]::NewGuid())
        New-Item -ItemType Directory -Path $dl | Out-Null
        if (Download-Executor $executorChoice $dl) {
            Set-Aside
            Move-Item -LiteralPath (Join-Path $dl 'contract-executor.jar'), (Join-Path $dl 'sc-api-support.jar') -Destination $PSScriptRoot
            $executorNotes.Add("contract-executor.jar and sc-api-support.jar replaced: $(Describe-Build (Jar-Build 'contract-executor.jar')).")
        } else {
            $executorAction = 'manual'
            $executorNotes.Add('The jars in this folder are kept.')
        }
        Remove-Item -LiteralPath $dl -Recurse -Force
    } elseif ($executorAction -eq 'restore') {
        Set-Aside
        Move-Item -LiteralPath 'contract-executor.jar.official' -Destination 'contract-executor.jar'
        Move-Item -LiteralPath 'sc-api-support.jar.official' -Destination 'sc-api-support.jar'
        $executorNotes.Add('The official executor jars are back.')
    }
    $build = Jar-Build 'contract-executor.jar'
    if ($build) {
        $min = Current 'api' 'executor_commit_min'
        $max = Current 'api' 'executor_commit_max'
        if ($min -and $build -lt [int]$min) { Set-ApiKey $out 'executor_commit_min' "$build"; $executorNotes.Add("config.ini: executor_commit_min=$build") }
        if ($max -and $max -ne '-1' -and $build -gt [int]$max) {
            Set-ApiKey $out 'executor_commit_max' "$build"
            $executorNotes.Add("config.ini: executor_commit_max=$build (it was ${max}: the node would not start this executor)")
        }
    }
    if ($executorAction -eq 'manual') {
        $executorNotes.Add('')
        $executorNotes.Add("To use the $executorChoice executor, with the node stopped:")
        if ($executorChoice -eq 'community') {
            $executorNotes.Add('  copy contract-executor.jar and sc-api-support.jar, both, from the csnode folder of the community')
            $executorNotes.Add("  package (or from $communityUrl/...) into $PSScriptRoot,")
        } else {
            $executorNotes.Add('  copy contract-executor.jar and sc-api-support.jar, both, from the official release')
            $executorNotes.Add("  ($officialUrl) into $PSScriptRoot,")
        }
        $executorNotes.Add('  then run this set-up again: it makes config.ini accept their build (executor_commit_min/max).')
    }
    $executorNotes.Add('')
    $executorNotes.Add('Updating this folder later: a new node.exe is not enough when its package brings a new executor.')
    $executorNotes.Add('Copy node.exe, contract-executor.jar and sc-api-support.jar together, then run the set-up again.')
}

# ASCII without a byte-order mark, as the package ships it
[System.IO.File]::WriteAllLines((Join-Path $PSScriptRoot $config), $out, (New-Object System.Text.ASCIIEncoding))
Write-Host "config.ini written; the previous one is $backup"
foreach ($n in $executorNotes) { Write-Host $n }

if ($first -eq 'github') {
    Write-Host ''
    Write-Host 'Downloading the published snapshot...'
    & $node --config-file $config --download-snapshot
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'The download did not finish. Run setup.bat again, or start the node: it can get the snapshot from other nodes.'
        exit 1
    }
}

$nodeArgs = @('--config-file', $config)
if ($convert) { $nodeArgs += '--convert-db' }
if ($start -eq 'yes') {
    Write-Host ''
    Write-Host 'Starting the node (Ctrl+C stops it and saves its state)...'
    & $node @nodeArgs
    exit $LASTEXITCODE
}
Write-Host ''
Write-Host "Start the node with: cd `"$PSScriptRoot`" ; node.exe $($nodeArgs -join ' ')"
