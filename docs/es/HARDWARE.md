# Hardware CAUCE

## Estado

**Aún sin hardware físico comprado.** Todo lo de firmware está probado
vía simulación y tests. El BME280 es la elección de referencia porque
es barato, se consigue en todas partes, habla I2C limpio, y trae
presión además de temperatura y humedad (±0.5 °C, ±3 %RH — números del
datasheet, no nuestros).

## Lista de materiales (por nodo) — borrador v0

| Componente | Cant | Propósito | Mínimo | Recomendado | Alternativas | Costo est. USD |
|---|---|---|---|---|---|---|
| Placa MCU | 1 | Cómputo + Wi-Fi + flash | ESP32-WROOM-32, 4 MB flash | ESP32-DevKitC WROOM-32E | ESP32-S3 (futuro), ESP8266 (limitado) | 4–7 |
| Sensor ambiental | 1 | T°, HR, presión | Módulo BME280 I2C 3.3 V | GY-BME280 con regulador | SHT31 (+BMP388), DHT22 (menor calidad) | 3–6 |
| Almacenamiento | — | Log local | Flash interna 4 MB (LittleFS) | igual | microSD (fase posterior) | 0 |
| Alimentación | 1 | Energía estable | USB 5 V / adapter ≥1A | cargador + power bank | panel solar + LiPo + TP4056 (autonomía) | 3–10 |
| Enclosure | 1 | Protección IP + radiación | IP54 ventilado, sin sol directo | mini pantalla Stevenson (impresa 3D) | PVC louvered | 2–8 |
| Cableado | — | I2C | dupont/JST cortos (<30 cm) | cable trenzado, pull-ups ya en módulo | — | 1 |
| Total | | | | | | **13–32** |

No casarse con un vendor: los módulos BME280 genéricos son
intercambiables, y el driver solo asume dirección I2C 0x76/0x77.

## Cableado de referencia (ESP32 DevKit → BME280)

```
ESP32 3V3  → BME280 VCC     (¡chequear que el módulo sea realmente 3.3 V!)
ESP32 GND  → BME280 GND
ESP32 GPIO21 (SDA) → BME280 SDI
ESP32 GPIO22 (SCL) → BME280 SCK
BME280 SDO → GND            (dirección 0x76; a 3V3 = 0x77)
```

Cableado en `main.cpp`: I2C a 100 kHz en pines 21/22.

## Consideraciones físicas (obligatorias para calidad científica)

- El sensor **nunca** a sol directo ni pegado a un muro que irradia
  calor; 1.5–2.5 m de altura anda.
- La pantalla de radiación es obligatoria si querés que los nodos sean
  comparables siquiera. Misma pantalla, misma altura, o no compares.
- Por nodo, anotar: fecha/hora de instalación, altura, orientación,
  superficie dominante del entorno (el contexto vive en `NodeConfig`).
- Sin certificado metrológico acá: estos números son para análisis
  comunitario comparativo, no meteorología oficial (ver
  CALIBRATION.md).
