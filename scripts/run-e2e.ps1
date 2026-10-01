# End-to-end integration test:
# firmware (real C++ SyncManager, socket HTTP) -> real FastAPI backend -> SQLite verification
param(
    [int]$Port = (Get-Random -Minimum 8800 -Maximum 9800),
    [int]$Records = 50
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$fw = Join-Path $root "firmware"
$backend = Join-Path $root "backend"

$Lang = if ($env:CAUCE_LANG -match '^(?i)en') { 'en' } else { 'es' }
$T = @{
    es = @{
        step1        = '== 1/4 build binario integracion =='
        buildFail    = 'build integration fallo'
        step2        = '== 2/4 levantar backend :{0} =='
        noHealth     = 'backend no respondio /healthz'
        ready        = '    backend listo'
        step3        = '== 3/4 ejecutar cliente de sincronizacion del nodo =='
        incomplete   = 'fases E2E incompletas ({0}/3)'
        step4        = '== 4/4 verificacion directa en SQLite =='
        noDb         = 'db no existe tras sincronizar: {0}'
        dbSize       = '    db size: {0}'
        sqliteFail   = 'verificacion SQLite fallo'
        passed       = '== E2E PASADO: 3 fases + base de datos =='
        serverLog    = '    server log: {0}'
    }
    en = @{
        step1        = '== 1/4 integration binary build =='
        buildFail    = 'integration build failed'
        step2        = '== 2/4 starting backend :{0} =='
        noHealth     = 'backend did not answer /healthz'
        ready        = '    backend ready'
        step3        = '== 3/4 running node sync client =='
        incomplete   = 'incomplete E2E phases ({0}/3)'
        step4        = '== 4/4 direct SQLite verification =='
        noDb         = 'db missing after sync: {0}'
        dbSize       = '    db size: {0}'
        sqliteFail   = 'SQLite verification failed'
        passed       = '== E2E PASSED: 3 phases + database =='
        serverLog    = '    server log: {0}'
    }
}[$Lang]

$mingwBin = "C:\Users\filip\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$env:Path = "$mingwBin;" + $env:Path
$env:CC = "$mingwBin\gcc.exe"
$env:CXX = "$mingwBin\g++.exe"

Write-Host $T.step1 -ForegroundColor Cyan
pio run -e integration --project-dir $fw | Out-Null
if ($LASTEXITCODE -ne 0) { throw $T.buildFail }
$exe = Join-Path $fw ".pio\build\integration\program.exe"

$db = Join-Path $env:TEMP ("cauce-e2e-" + [guid]::NewGuid().ToString("N") + ".sqlite")
$env:CAUCE_DB_PATH = $db

Write-Host ($T.step2 -f $Port) -ForegroundColor Cyan
$serverLog = Join-Path $env:TEMP "cauce-e2e-server.log"
$server = Start-Process -FilePath python `
    -ArgumentList "-m","uvicorn","cauce_server.main:app","--port","$Port" `
    -WorkingDirectory $backend -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $serverLog -RedirectStandardError ($serverLog + ".err")
try {
    $ready = $false
    foreach ($i in 1..20) {
        Start-Sleep -Milliseconds 500
        try {
            $null = Invoke-WebRequest "http://127.0.0.1:$Port/healthz" -UseBasicParsing -TimeoutSec 2
            $ready = $true; break
        } catch {}
    }
    if (-not $ready) { throw $T.noHealth }
    Write-Host $T.ready -ForegroundColor DarkGray

    Write-Host $T.step3 -ForegroundColor Cyan
    $output = & $exe "http://127.0.0.1:$Port/v1/sync" $Records 2>&1
    $output | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    $oks = @($output | Where-Object { $_ -match "^E2E_OK" })
    if ($oks.Count -lt 3) {
        $output | Where-Object { $_ -match "E2E_FAIL" } | Write-Host
        throw ($T.incomplete -f $oks.Count)
    }

    Write-Host $T.step4 -ForegroundColor Cyan
    if (-not (Test-Path $db)) { throw ($T.noDb -f $db) }
    Write-Host ($T.dbSize -f (Get-Item $db).Length) -ForegroundColor DarkGray
    $verify = @"
import os, sqlite3, sys
lang = os.environ.get("CAUCE_LANG", "es")[:2].lower()
MSG = {
    "es": {"checking": "verificando", "dups": "duplicados o faltantes", "nobatches": "faltan registros de sync_batches"},
    "en": {"checking": "checking", "dups": "duplicates or missing rows", "nobatches": "missing sync_batches records"},
}[lang if lang in ("es", "en") else "es"]
print(MSG["checking"] + ":", r"$db", "size=", os.path.getsize(r"$db"))
conn = sqlite3.connect(r"$db")
total = conn.execute("SELECT COUNT(*) FROM measurements").fetchone()[0]
distinct = conn.execute("SELECT COUNT(DISTINCT sequence) FROM measurements WHERE node_id='CAUCE-E2E'").fetchone()[0]
maxseq = conn.execute("SELECT MAX(sequence) FROM measurements WHERE node_id='CAUCE-E2E'").fetchone()[0]
batches = conn.execute("SELECT COUNT(*) FROM sync_batches WHERE node_id='CAUCE-E2E'").fetchone()[0]
print(f"E2E_DB rows={total} distinct={distinct} max_seq={maxseq} batches={batches}")
expected = $Records + 5
assert total == distinct == expected == maxseq, f"{MSG['dups']}: total={total} esperado={expected}"
assert batches >= 2, MSG["nobatches"]
print("E2E_OK stage=db_verification rows=", total)
"@
    $verify | python -
    if ($LASTEXITCODE -ne 0) { throw $T.sqliteFail }

    Write-Host $T.passed -ForegroundColor Green
    Write-Host ($T.serverLog -f $serverLog) -ForegroundColor DarkGray
    exit 0
} finally {
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $db, "$db-wal", "$db-shm" -ErrorAction SilentlyContinue
}
