# Despliegue CAUCE

Cómo ir de un checkout limpio a un piloto operativo (5–10 nodos más el
servidor central). En orden, sin saltear pasos.

## 0. Requisitos

| Componente | Versión | Propósito |
|---|---|---|
| Python | ≥3.10 (probado 3.12) | CLI PlatformIO + backend |
| PlatformIO Core | ≥6.1 | build/test firmware |
| GCC MinGW-w64 (Windows) / gcc (Linux) | ≥9 | tests nativos y binarios |
| Docker (opcional) | — | contenedor backend |
| Hardware por nodo | ver HARDWARE.md | ESP32 + BME280 |

## 1. Verificar el entorno desde cero

```powershell
git clone <repo> && cd CAUCE
pip install platformio==6.1.19
cd firmware
pio test -e native        # esperado 317 ok
pio run -e esp32dev       # esperado SUCCESS
```

Backend:

```powershell
cd backend
pip install --require-hashes -r requirements.lock
python -m pytest tests -q # esperado 495 passed
```

Integración nodo↔servidor:

```powershell
.\scripts\run-e2e.ps1    # esperado E2E PASSED
```

Gate de un comando: `.\scripts\verify-all.ps1` (`CAUCE_LANG=en` para
salida en inglés).

## 2. Servidor central

```powershell
# Directo
cd backend
$env:CAUCE_DB_PATH = "C:\cauce\data\central.sqlite"
$env:CAUCE_SYNC_TOKEN = "<shared-node-secret>"
uvicorn cauce_server.main:app --host 0.0.0.0 --port 8000

# Docker
docker compose -f deployment/docker-compose.yml up -d --build
```

Chequear `GET http://<server>:8000/healthz`; el dashboard central está
en `/`. Si `/healthz` no responde, arreglar eso antes de tocar ningún
nodo.

### Credenciales, y cuál es cuál

Tres secretos, y confundir cualquiera de los dos primeros elimina una separación
que quieres conservar:

| Variable | Para qué | Si no está definida |
|---|---|---|
| `CAUCE_SYNC_TOKEN` | Autentican a los nodos en `/v1/sync`. Compartido | El sync queda abierto, que es el comportamiento previo a la autenticación |
| `CAUCE_API_TOKEN` | La API de administración. Compartido | Toda escritura responde `503 admin_api_not_configured` |
| `CAUCE_CA_KEY` | Una **semilla** Ed25519 que firma certificados de nodo | Los tres endpoints de certificados responden `503 certificate_authority_not_configured` |

`CAUCE_CA_KEY` es una semilla de 32 bytes en hex, y deliberadamente **no** es el mismo
valor que `CAUCE_API_TOKEN`. El token de admin ya puede cambiar la calibración, emitir
credenciales con scope y retirar un nodo; dejar que además emita certificados haría que una
sola filtración bastara para suplantar cualquier nodo de la flota.

```bash
python -c "import secrets; print(secrets.token_hex(32))"
```

Los certificados duran 90 días por defecto (`CAUCE_CERT_VALIDITY_SECONDS`). Por encima de
366 días la API lo rechaza en vez de recortarlo, para que una errata no emita un
certificado que sobreviva al despliegue. **No hay rotación de la clave de CA** en este
código: rotar significa cambiar la clave que tiene cada verificador, que es un paso manual
deliberado. Perder la clave implica reemitir todos los certificados.

## 2b. TLS delante del central

El backend habla HTTP plano a propósito y **no se publica a la LAN**. Caddy
termina TLS y es el único puerto publicado:

```bash
cd deployment
export CAUCE_DOMAIN=cauce.example.org      # el hostname que van a usar los nodos
export ACME_EMAIL=ops@cauce.example.org    # para que Let's Encrypt te avise
docker compose up -d --build
```

Qué compra y qué cuesta:

- `http://CAUCE_DOMAIN` redirige a `https://CAUCE_DOMAIN`.
- El central responde solo en `127.0.0.1:8000`, así un operador puede
  depurar desde el host mientras la red ve el 443 y nada más.
- Los lotes van firmados por dispositivo (HMAC-SHA256) además de cifrados:
  el TLS no reemplaza la firma, y la firma no reemplaza el TLS.

**Modos de certificado.** `CAUCE_TLS_MODE` elige uno, y dejarlo vacío es el
default:

- **Vacío (recomendado).** `tls` sin argumento hace que Caddy resuelva un
  certificado ACME real para `CAUCE_DOMAIN` y lo renueve solo. Lo único que
  hace falta es un nombre público que apunte al host y los puertos 80 y 443
  alcanzables.
- **`internal`.** Para un piloto sin DNS público. Caddy emite desde su
  propia CA local y hay que confiar en esa CA en todas partes:

  ```bash
  export CAUCE_TLS_MODE=internal
  docker compose up -d
  docker compose cp caddy:/data/caddy/pki/authorities/local/root.crt ./cauce-root.crt
  ```

  Instalá `cauce-root.crt` en cada nodo y en la máquina del operador; si no,
  los nodos rechazan el lote.

No hay un tercer modo a propósito. Fijar un certificado autofirmado por
nodo es más trabajo operativo que cualquiera de los dos anteriores, y un
certificado que nadie rota termina siendo un certificado en el que nadie
confía.

Del lado del nodo: apuntá `sync_server_url` a `https://CAUCE_DOMAIN`, e
instalá el certificado raíz en el ESP32 solo si elegiste `internal`. Desde
ahí queda satisfecha la casilla "backend accesible por TLS".

alcanzable por TLS".

## 3. Preparar cada nodo


1. **Identidad**: un `node_id` único (`CAUCE-001`, …). Jamás dos nodos
   comparten uno — todo el diseño de sync lo asume.
2. **Config**: copiar `configs/node.example.conf`, completar identidad,
   ubicación, contexto, intervalo de muestreo.
3. **Hardware**: ensamblar según HARDWARE.md (BME280 en I2C 21/22, dir
   0x76, pantalla de radiación obligatoria — no recomendada,
   obligatoria).
4. **Flash**:
   ```powershell
   pio run -e esp32dev -t upload
   pio device monitor      # logs de boot estructurados
   ```
5. **Secretos**: Wi-Fi y sync token entran por la UI local
   (`http://<node-ip>/` → Configuration) con el admin token del nodo.
   La flash guarda el hash, nunca el secreto.

## 4. Validación en campo

- El dashboard del nodo muestra `SENSOR_DISCOVERED`, tarjetas con
  calidad `VALID`, y la gráfica llenándose. Si la gráfica queda vacía,
  frenar — el problema está en el sensor, no en el servidor.
- Registrar el sitio y asignar el nodo en el server
  (`POST /v1/sites`, `PUT /v1/nodes/{id}/site`).
- Confirmar ingesta: `GET /v1/nodes/{id}/measurements?limit=5`
  devuelve tus filas.
- Co-localizar los nodos ≥48h para comparabilidad entre nodos
  (CALIBRATION.md). Es la exactitud más barata que vas a comprar.

## 5. Evaluación de intervenciones

```http
POST /v1/interventions {"site_id":"...","kind":"shade",
                        "start_utc_ms":1787356800000}
GET  /v1/analytics/before-after?intervention_id=1&node_id=CAUCE-001&variable=air_temperature
```

No interpretar nada hasta `sufficient_sample:true`. Bajo 30 muestras
por período, el endpoint te dice que está adivinando — hacerle caso.

## 6. Mantenimiento

- Logs serie USB estructurados (grep-friendly, códigos de evento no
  prosa).
- Salud remota vía `/v1/nodes/{id}` y el panel central.
- Los datos del nodo rotan solos; la historia permanente vive en la
  central.
- Updates de firmware: **la capa de decisión OTA está hecha, el
  flasheo real espera hardware** — hoy significa acceso físico.
  Planificar las visitas consecuentemente.

## 7. Checklist de aceptación del piloto

- [ ] `pio test -e native` en verde
- [ ] Backend con token + rate limit configurados
- [ ] Central alcanzable por TLS a través del terminador Caddy, con el
      certificado raíz instalado en cada nodo
- [ ] `run-e2e.ps1` en verde tras cualquier cambio de protocolo
- [ ] Cada nodo: identificado, geo-localizado, calidad VALID llegando
      al centro, metadata de instalación anotada
