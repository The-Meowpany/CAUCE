# Hardware CAUCE

## Estado

**No hay hardware físico adquirido todavía.** Todo el firmware se validó
con simulación y tests. El BME280 es la implementación de referencia
elegida por: precisión razonable (±0.5 °C, ±3 %RH), presión barométrica,
bus I2C limpio, disponibilidad y precio.

## Bill of Materials (por nodo) — draft v0

| Componente | Cant | Propósito | Mínimo | Recomendado | Alternativas | Costo aprox. USD |
|---|---|---|---|---|---|---|
| MCU board | 1 | Cómputo + Wi-Fi + flash | ESP32-WROOM-32, 4 MB flash | ESP32-DevKitC WROOM-32E | ESP32-S3 (futuro), ESP8266 (limitado) | 4–7 |
| Sensor ambiental | 1 | T°, HR, presión | BME280 módulo I2C 3.3 V | GY-BME280 con regulador | SHT31 (+BMP388), DHT22 (menor calidad) | 3–6 |
| Almacenamiento | — | Log local | Flash interna 4 MB (LittleFS) | ídem | microSD (fase posterior) | 0 |
| Fuente | 1 | Alimentación estable | USB 5 V / adaptador 5V≥1A | cargador + PowerBank | panel solar + LiPo + TP4056 (autonomía) | 3–10 |
| Carcasa | 1 | Protección IP + radiación | IP54 ventilada, sin exposición directa al sol | pantalla de radiación tipo Stevenson mini impresa 3D | PVC con lamas | 2–8 |
| Cableado | — | I2C | dupont/jst cortos (<30 cm) | cable trenzado + pull-ups ya en módulo | — | 1 |
| Total | | | | | | **13–32** |

Evitar dependencia de proveedor único: los módulos BME280 genéricos son
intercambiables; el driver solo asume I2C addr 0x76/0x77.

## Cableado referencia (ESP32 DevKit → BME280)

```
ESP32 3V3  → BME280 VCC     (¡verificar módulo 3.3 V!)
ESP32 GND  → BME280 GND
ESP32 GPIO21 (SDA) → BME280 SDI
ESP32 GPIO22 (SCL) → BME280 SCK
BME280 SDO → GND            (dirección 0x76; a 3V3 = 0x77)
```

Configurado en `main.cpp`: bus I2C a 100 kHz pines 21/22.

## Consideraciones físicas (obligatorias para calidad científica)

- El sensor **nunca** al sol directo ni cerca de fuentes de calor/paredes
  que radien; altura sugerida 1.5–2.5 m.
- Radiación screen obligatoria para comparabilidad entre nodos.
- Anotar por nodo: fecha/hora de instalación, altura, orientación,
  superficie dominante alrededor (contexto va en `NodeConfig`).
- Sin certificado metrológico: los valores son para análisis comparativo
  comunitario, no para uso meteorológico oficial (ver CALIBRATION.md).
