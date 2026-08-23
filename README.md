# CAUCE — Red Comunitaria de Microestaciones Climáticas

Infraestructura de monitoreo ambiental **offline-first** para adaptación
climática en Canelones, Uruguay. Cada nodo es una microestación basada en
ESP32 que mide, valida, almacena localmente y sirve sus datos por Wi-Fi sin
requerir Internet.

> **Estado actual**: núcleo de firmware funcional con suite de tests en verde
> y build ESP32 verificado. Lee [STATUS.md](STATUS.md) antes de asumir que
> algo existe: el documento distingue explícitamente lo implementado de lo
> pendiente.

## Filosofía

```
offline-first · local-first · modular · reproducible · observable · fail-safe
```

Un componente fallido no arrastra al sistema:

- Sensor desconectado → se registra `MISSING`, los demás sensores siguen.
- Sin Internet → el nodo mide y almacena igual.
- Apagón → los registros tienen CRC; la cola parcial corrupta se aísla.
- Config inválida → rollback al backup anterior o defaults seguros.

## Estructura

```
CAUCE/
├── firmware/            Firmware ESP32 (PlatformIO)
│   ├── lib/cauce_hal/      Abstracción de hardware (interfaces)
│   ├── lib/cauce_core/     Dominio: medición, validación, storage, config
│   ├── lib/cauce_drivers/  Drivers: simulado + BME280 referencia
│   ├── lib/cauce_app/      Scheduler de mediciones
│   ├── src/main.cpp        Composition root (+ demo de simulación en PC)
│   └── test/               Suite Unity (42 tests, corren en PC)
├── protocol/v1/         Especificación del protocolo de datos v1
├── docs/                Documentación técnica
├── configs/             Configuración de ejemplo
├── scripts/             Scripts de build/test
└── poolpa/              Proyecto independiente (no relacionado)
```

## Inicio rápido (sin hardware)

```powershell
# Requisitos: Python 3.10+, PlatformIO (pip install platformio),
# GCC MinGW-w64 en PATH (tests nativos) — ver docs/TESTING.md
cd firmware
pio test -e native          # 42 tests en PC
pio run -e native           # compila demo de simulación
.\.pio\build\native\program.exe   # corre escenarios con fallas simuladas
```

El demo ejecuta ~50 ciclos con eventos inyectados: sensor congelado,
fuera de rango, desconexión, recuperación. Observa cómo la calidad de datos
pasa de `VALID` a `SUSPECT`, se registran `MISSING`, y el nodo continúa.

## Compilar para ESP32

```powershell
cd firmware
pio run -e esp32dev         # genera .pio/build/esp32dev/firmware.bin
pio run -e esp32dev -t upload   # requiere placa conectada
```

Sin hardware físico aún probado en bench: el binario compila y enlaza; la
validación con placa real es pendiente declarada.

## Documentación

| Documento | Contenido |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Capas, módulos, decisiones |
| [PROTOCOL.md](protocol/v1/PROTOCOL.md) | Formato de registro binario v1 |
| [DATA_MODEL.md](docs/DATA_MODEL.md) | Measurement, calidad, almacenamiento |
| [HARDWARE.md](docs/HARDWARE.md) | BOM, cableado I2C |
| [CALIBRATION.md](docs/CALIBRATION.md) | Modelo offset/escala y límites honestos |
| [TESTING.md](docs/TESTING.md) | Cómo correr tests, cobertura real |
| [SECURITY.md](docs/SECURITY.md) | Postura de seguridad actual |
| [API.md](docs/API.md) | Endpoints /api/v1, auth, streaming |
| [SYNC.md](docs/SYNC.md) | Contrato de sincronizaci�n nodo?central |
| [STATUS.md](STATUS.md) | Qué es real y qué falta (leer primero) |

## Licencia

MIT — ver [LICENSE](LICENSE).
