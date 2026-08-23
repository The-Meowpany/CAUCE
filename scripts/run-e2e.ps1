# Test de integracion extremo a extremo:
# firmware(SyncManager C++ real, HTTP por sockets) -> backend FastAPI real -> verificacion en SQLite
param(
    [int]$Port = (Get-Random -Minimum 8800 -Maximum 9800),
    [int]$Records = 50
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$fw = Join-Path $root "firmware"
$backend = Join-Path $root "backend"

$mingwBin = "C:\Users\filip\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$env:Path = "$mingwBin;" + $env:Path
$env:CC = "$mingwBin\gcc.exe"
$env:CXX = "$mingwBin\g++.exe"

Write-Host "== 1/4 build binario integracion ==" -ForegroundColor Cyan
pio run -e integration --project-dir $fw | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build integration fallo" }
$exe = Join-Path $fw ".pio\build\integration\program.exe"

$db = Join-Path $env:TEMP ("cauce-e2e-" + [guid]::NewGuid().ToString("N") + ".sqlite")
$env:CAUCE_DB_PATH = $db

Write-Host "== 2/4 levantar backend :$Port ==" -ForegroundColor Cyan
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
    if (-not $ready) { throw "backend no respondio /healthz" }
    Write-Host "    backend listo" -ForegroundColor DarkGray

    Write-Host "== 3/4 ejecutar cliente de sincronizacion del nodo ==" -ForegroundColor Cyan
    $output = & $exe "http://127.0.0.1:$Port/v1/sync" $Records 2>&1
    $output | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    $oks = @($output | Where-Object { $_ -match "^E2E_OK" })
    if ($oks.Count -lt 3) {
        $output | Where-Object { $_ -match "E2E_FAIL" } | Write-Host
        throw "fases E2E incompletas ($($oks.Count)/3)"
    }

    Write-Host "== 4/4 verificacion directa en SQLite ==" -ForegroundColor Cyan
    if (-not (Test-Path $db)) { throw "db no existe tras sincronizar: $db" }
    Write-Host ("    db size: " + (Get-Item $db).Length) -ForegroundColor DarkGray
    $verify = @"
import os, sqlite3, sys
print("verificando:", r"$db", "size=", os.path.getsize(r"$db"))
conn = sqlite3.connect(r"$db")
total = conn.execute("SELECT COUNT(*) FROM measurements").fetchone()[0]
distinct = conn.execute("SELECT COUNT(DISTINCT sequence) FROM measurements WHERE node_id='CAUCE-E2E'").fetchone()[0]
maxseq = conn.execute("SELECT MAX(sequence) FROM measurements WHERE node_id='CAUCE-E2E'").fetchone()[0]
batches = conn.execute("SELECT COUNT(*) FROM sync_batches WHERE node_id='CAUCE-E2E'").fetchone()[0]
print(f"E2E_DB rows={total} distinct={distinct} max_seq={maxseq} batches={batches}")
expected = $Records + 5
assert total == distinct == expected == maxseq, f"duplicados o faltantes: total={total} esperado={expected}"
assert batches >= 2, "faltan registros de sync_batches"
print("E2E_OK stage=db_verification rows=", total)
"@
    $verify | python -
    if ($LASTEXITCODE -ne 0) { throw "verificacion SQLite fallo" }

    Write-Host "== E2E PASADO: 3 fases + base de datos ==" -ForegroundColor Green
    Write-Host ("    server log: " + $serverLog) -ForegroundColor DarkGray
    exit 0
} finally {
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $db, "$db-wal", "$db-shm" -ErrorAction SilentlyContinue
}
