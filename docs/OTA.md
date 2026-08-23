# OTA — actualización de firmware

## Arquitectura (lógica implementada y testeada)

```
OtaManager (FSM)                    IManifestSource   → catálogo de releases
  IDLE ──intervalo──▶ CHECKING      IFirmwareReader   → descarga por chunks
  CHECKING: catálogo + semver       IFirmwareInstaller→ escritura/commit
    ├─ sin novedad ──▶ UP_TO_DATE   (hooks: heap libre, batería, reboot)
    ├─ gates bloquean ▶ CHECK_FAILED
    └─ nueva versión ▶ DOWNLOADING
         DOWNLOADING: begin→chunks(sha256 streaming)→finish
           ├─ hash/tamaño OK y installer confirma ▶ REBOOT_PENDING
           ├─ hash o tamaño inválido ▶ VERIFY_FAILED (+abort)
           └─ escritura falla       ▶ INSTALL_FAILED (+abort)
```

## Reglas anti-ladrillo

1. **Nunca se toca la imagen actual**: el instalador escribe en la
   partición alternativa; `finishInstall()` representa
   `esp_ota_set_boot_partition` (marcar nueva como candidata).
2. **Verificación previa al reboot**: SHA-256 en streaming sobre los bytes
   recibidos debe coincidir EXACTAMENTE con el manifiesto; tamaño también.
3. **Gates de seguridad** antes de descargar: heap libre mínimo
   (default 40 KB) y batería mínima (`setMinBatteryV`; deshabilitado si 0).
4. **Rollback operativo**: tras reiniciar, el nodo debe confirmar salud
   (patrón boot-counter / `esp_ota_mark_app_valid_cancel_rollback`).
   La integración con ese mecanismo nativo es la parte pendiente de placa.
5. Sin manifest válido o versión igual/menor → no hay descarga.

## API del nodo

```cpp
OtaManager mgr(catalog, reader, installer, clock, logger);
mgr.setFirmwareVersion(Versions::kFirmware);
mgr.setSafetyHooks(freeHeapFn, batteryFn);   // opcional
mgr.setMinBatteryV(3.3f);
mgr.setRebootHook(esp_restart);
mgr.tick();                                   // no bloqueante por diseño
```

## Contrato del manifiesto

`IManifestSource.fetchLatest(current)` devuelve `{version, sha256Hex,
url, totalSize}` o false. El backend central puede servirlo vía
`GET /v1/ota/manifest` leyendo un archivo de releases (implementación
del lector HTTP del lado ESP32: pendiente de placa).

## Tests (10, host)

Comparación semver · sha256 streaming ≡ one-shot · flujo feliz completo ·
hash corrupto → abort+VERIFY_FAILED · tamaño excedido · misma versión →
sin descarga · sin release · rechazo del instalador · compuerta de heap ·
compuerta de batería.
