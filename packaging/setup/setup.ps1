# Guided set-up of the node in this folder (Windows): the same questions and the same config.ini as
# setup.sh. Started by setup.bat; "setup.bat --defaults" takes every default without asking.
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

# 5. start now
$start = Ask '5. Start the node when the set-up is done? (yes/no)' 'yes' @('yes', 'no')
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
# ASCII without a byte-order mark, as the package ships it
[System.IO.File]::WriteAllLines((Join-Path $PSScriptRoot $config), $out, (New-Object System.Text.ASCIIEncoding))
Write-Host "config.ini written; the previous one is $backup"

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
