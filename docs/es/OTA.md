# OTA CAUCE — actualizaciones de firmware

## Arquitectura (capa de decisión implementada y probada)

```
OtaManager (FSM)                     IManifestSource  → catálogo de releases
  IDLE ──interval──▶ CHECKING        IFirmwareReader  → descarga por chunks
  CHECKING: catálogo + semver        IFirmwareInstaller → escritura/commit
    ├─ nada nuevo ──▶ UP_TO_DATE     hooks: heap libre, batería, reboot
    ├─ gates bloqueados ▶ CHECK_FAILED
    └─ versión nueva ▶ DOWNLOADING
         DOWNLOADING: begin→chunks(sha256 streaming)→finish
           ├─ hash+tamaño OK, installer confirma ▶ REBOOT_PENDING
           ├─ hash/tamaño inválido ▶ VERIFY_FAILED (+abort)
           └─ fallo de escritura        ▶ INSTALL_FAILED (+abort)
```

Diez tests host cubren esta FSM, más tres para el parser JSON del
manifiesto. El backend de flasheo ESP32 también está implementado
(`Esp32Ota`: fetch HTTP de manifiesto, escritura `Update` en streaming
a la partición alterna, hook de reboot) y cross-compila — pero ninguna
placa lo corrió aún, así que todo el camino de flasheo se trata como no
validado hasta que el banco lo diga.

## Reglas anti-brick

1. **La imagen en ejecución nunca se toca.** El installer escribe en la
   partición alterna; `finishInstall()` significa
   `esp_ota_set_boot_partition` (marcar la imagen nueva como candidata,
   no comprometerse a ciegas).
2. **Verificar antes del reboot.** El SHA-256 en streaming sobre los
   bytes recibidos tiene que matchear el manifiesto exacto, y el tamaño
   también. Un byte mal, sin reboot.
3. **Gates de seguridad primero.** Heap libre mínimo (default 40 KB) y
   batería mínima (`setMinBatteryV`; 0 desactiva el check) se evalúan
   antes de que baje un solo byte.
4. **Probar salud tras reiniciar.** El nodo debe confirmar que está vivo
   (patrón boot-counter / `esp_ota_mark_app_valid_cancel_rollback`).
   Cablear ese mecanismo nativo sigue pendiente de placa — hasta que una
   placa lo confirme, los updates son con acceso físico en la práctica.
5. Sin manifiesto, o versión igual/menor, significa ninguna descarga. Un
   `ota_manifest_url` vacío mantiene OTA dormido: el catálogo reporta
   que no hay release y el manager se queda quieto.

## API del nodo

```cpp
OtaManager mgr(catalog, reader, installer, clock, logger);
mgr.setFirmwareVersion(Versions::kFirmware);
mgr.setSafetyHooks(freeHeapFn, batteryFn);   // opcional
mgr.setMinBatteryV(3.3f);
mgr.setRebootHook(esp_restart);
mgr.tick();                                   // no-bloqueante por diseño
```

`tick()` nunca bloquea: un chunk por llamada, estado en el manager. En
el nodo el dashboard sigue arriba durante un check — las descargas
bloqueantes son un backlog conocido (STATUS.md #3), no una sorpresa.

## Contrato del manifiesto

`IManifestSource.fetchLatest(current)` retorna `{version, sha256Hex,
url, totalSize}` o false. El backend central lo sirve vía
`GET /v1/ota/manifest` desde un archivo JSON de releases
(`CAUCE_OTA_RELEASES`): `{version, sha256, url, total_size}` más un
`hmac` por nodo cuando `node_id` es de un dispositivo provisionado
(HMAC sobre `version|url|total_size` con la device key). Sin archivo de
releases el endpoint responde 404 y los nodos se quedan quietos.

Del lado del nodo, `Esp32ManifestSource` hace fetch a esa URL (con
`?node_id=`) y la parsea con `parseOtaManifestJson` sin dependencias —
el mismo parser que cubren los tests host, así que el manejo del JSON
está probado aunque el flasheo espera hardware.

## Tests (13, host)

Pares semver · streaming ≡ one-shot sha256 · happy path completo (un
solo fetch de catálogo) · abort por hash corrupto · tamaño excedido ·
misma versión skip · sin release · rechazo del installer · gate de heap ·
gate de batería.
