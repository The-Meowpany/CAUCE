# Hardware CAUCE

## Estado

**Aún sin hardware físico comprado.** Cada driver de abajo está cubierto por tests en host
contra buses simulados (I2C, UART, ADC, 1-Wire) y el firmware compila para ESP32, pero nada de
esto estuvo todavía en una placa. Las cifras de datasheet son del fabricante, no nuestras.

## Estación piloto (8 nodos idénticos)

Todos los sensores son opcionales y están **apagados por defecto**: un sensor se habilita con
su clave de config solo donde está instalado, y un nodo al que le falta uno sigue midiendo todo
lo demás. Un sensor habilitado que no responde al arrancar queda registrado como
`SENSOR_ABSENT` y se omite.

| Variable(s) | Componente | Bus / pin | Clave de config | Driver |
|---|---|---|---|---|
| air_temperature, relative_humidity, pressure | BME280 | I2C 0x76/0x77 | siempre activo | `Bme280Driver` |
| illuminance | BH1750 | I2C 0x23 | `light_enabled` | `Bh1750Driver` |
| pm1, pm2_5, pm4, pm10 | Sensirion SPS30 | I2C 0x69 | `pm_enabled` | `Sps30Driver` |
| co2 | Winsen MH-Z19C (NDIR) | UART2 RX 16 / TX 17, 9600 baudios | `co2_enabled` | `Mhz19Driver` |
| uv_index, uva_index, uvb_index | Vishay VEML6075 | I2C 0x10 | `uv_enabled` | `Veml6075Driver` |
| lightning_strikes, lightning_distance | AS3935 | I2C 0x03 + IRQ GPIO25 | `lightning_enabled` | `As3935Driver` |
| wind_speed | anemómetro de cazoletas (reed/Hall) | GPIO4, flanco de bajada | `wind_enabled` | `HallAnemometerDriver` |
| wind_direction | veleta de escalera de resistencias, pull-up 10k | ADC GPIO34 | `vane_enabled` | `WindVaneDriver` |
| rainfall | pluviómetro de balancín | GPIO27, flanco de bajada | `rain_enabled` | `RainGaugeDriver` |
| soil_temperature | DS18B20, pull-up 4,7k | 1-Wire GPIO13 | `soil_temperature_enabled` | `Ds18b20Driver` |
| soil_moisture | sonda capacitiva | ADC GPIO35 | `soil_moisture_enabled` | `AnalogSensorDriver` |
| leaf_wetness | rejilla resistiva/capacitiva | ADC GPIO32 | `leaf_wetness_enabled` | `AnalogSensorDriver` |
| solar_irradiance (relativa) | BPW34 + etapa de transimpedancia | ADC GPIO39 | `irradiance_enabled` | `AnalogSensorDriver` |
| soil_water_potential | bloque de yeso + referencia 10k | ADC GPIO33; extremos manejados por GP0/GP1 de un MCP23008 en I2C 0x20 con `soil_potential_ac=1` | `soil_potential_enabled`, `soil_potential_ac` | `GypsumAcDriver` (AC) o `AnalogSensorDriver` (DC) |
| battery_voltage | divisor resistivo | ADC GPIO36 | `battery_monitor_enabled` | `AnalogSensorDriver` |
| (reloj) | RTC DS3231 | I2C 0x68 | `rtc_enabled` | `Ds3231Rtc` |
| (archivo) | microSD 16 GB high-endurance | VSPI, CS GPIO15 | `sd_archive_enabled` | `ArchiveManager` + `Esp32SdCard` |
| tvoc | CCS811 (opcional, fuera del BOM del piloto) | I2C 0x5A | `voc_enabled` | `Ccs811Driver` |

Notas que cambian cómo se pueden leer los números:

- **El bloque de yeso** se lee con polaridad alterna cuando `soil_potential_ac=1`. Leído como
  divisor en continua, la corriente atraviesa el bloque siempre en el mismo sentido, lo que lo
  polariza y desplaza la lectura a lo largo de una temporada. Con el expansor, GP0 maneja el
  extremo de la referencia de 10k y GP1 el extremo lejano del bloque; cada lectura es una
  conversión en cada sentido y luego ambos extremos en bajo, así la carga neta es cero. Las dos
  conversiones se vuelven a expresar en los milivoltios que habría leído el divisor en continua,
  por lo que `soil_potential_kpa_per_mv` y `soil_potential_offset_kpa` conservan su significado y
  un ajuste hecho en un nodo DC sigue valiendo. Se usa el MCP23008 (pines de dirección a masa)
  porque sus salidas push-pull pueden entregar la corriente del divisor; el lado alto débil de un
  PCF8574 no puede. Una lectura cuyas dos mitades no suman la alimentación se rechaza en vez de
  informarse, porque así se ve un extremo sin manejar.

- El **CO2** sale solo de la celda NDIR. El "eCO2" del CCS811 es una estimación derivada de
  VOC y nunca se publica. La corrección automática de línea base del MH-Z19C se apaga al
  arrancar: la calibración es el procedimiento de co-localización, no un proceso en segundo
  plano.
- **PM**: el SPS30 necesita 30 s de ventilador antes de su primer frame; las lecturas
  anteriores se reportan como "inicializando" y no se guardan.
- **UV**: la conversión del VEML6075 usa los coeficientes de Vishay para sensor al aire
  libre. Detrás de una ventana o difusor hay que reajustarlos.
- **Rayos**: una descarga es una interrupción, contada en el loop (nunca en la ISR). Un
  intervalo sin descargas reporta 0 descargas y *ninguna* distancia; cero significaría "encima".
- La **irradiancia** de un fotodiodo es relativa hasta ajustar su ganancia contra un
  piranómetro.
- **Humedad de suelo / mojado foliar** son porcentajes entre una lectura seca y una húmeda, no
  contenido volumétrico de agua.
- **Lluvia** es lámina por intervalo de muestreo; los totales diarios son sumas en la central.
- **Validación**: los ceros de lluvia, luz, UV, irradiancia, viento y rayos, y una veleta que
  mantiene su rumbo, no se marcan como sensor trabado: son el estado de reposo.

## Mapa de pines (ESP32-WROOM-32)

| GPIO | Uso |
|---|---|
| 4 | pulsos del anemómetro (pull-up interno) |
| 5, 18, 19, 23 | SX1276 NSS, SCK, MISO, MOSI (VSPI) |
| 13 | 1-Wire (DS18B20) |
| 14 | SX1276 RESET |
| 15 | chip select de la microSD |
| 16, 17 | UART2 RX/TX (MH-Z19C) |
| 21, 22 | I2C SDA/SCL, 100 kHz |
| 25 | IRQ del AS3935 |
| 26 | SX1276 DIO0 |
| 27 | pulsos del pluviómetro (pull-up interno) |
| 32, 33, 34, 35, 36, 39 | ADC1: mojado foliar, potencial hídrico, veleta, humedad de suelo, batería, irradiancia |

Las entradas analógicas usan **solo ADC1**. ADC2 se comparte con la radio Wi-Fi y devuelve
errores mientras el Wi-Fi está activo; por eso `Esp32AnalogInput` rechaza cualquier pin fuera
de 32..39. Los GPIO 0, 2 y 12 son pines de arranque y quedan libres. El GPIO15 también lo es, pero solo
elige la salida del log de arranque y se lee en alto, que es también el reposo del chip select de
la SD.

## Módulo LoRa SX1276 - pines que usa el firmware

`main.cpp` construye la radio cuando `lora_enabled=1`. Los pines están en código y no en el
archivo de config a propósito: un juego de pines equivocado no falla ruidosamente, simplemente
no habla con nada.

| Señal | GPIO | Notas |
|---|---|---|
| SCK | 18 | VSPI |
| MISO | 19 | VSPI |
| MOSI | 23 | VSPI |
| NSS | 5 | chip select, activo en bajo |
| RESET | 14 | se mantiene en bajo un instante en `begin()` |
| DIO0 | 26 | TxDone / RxDone |
| DIO1 | sin cablear | el driver consulta los flags de IRQ |
| BUSY | sin cablear | es una señal del SX126x; el SX1276 no la tiene |

El SPI corre a **1 MHz**. Comprar módulos SX1276/RFM95W: las placas con SX1262 (Heltec WiFi
LoRa 32 V3 y similares) necesitan otro driver.

La región sale de `lora_region` como **nombre** (`EU868`, `US915`, `AU915`, `AS923`) y cada
una se traduce a su frecuencia en `kLoRaDefaultsFor`. El valor por defecto es **AU915**, la
banda que corresponde a Uruguay. Una región que este build no reconoce se rechaza y se reporta
como `LORA_REGION_UNKNOWN`; no cae a otra banda. Verificar potencia, ciclo de trabajo y uso de
canales con el regulador nacional (URSEC en Uruguay) antes del despliegue.

Las tramas se firman según `lora_frame_algorithm`: `hmac` (por defecto, con `sync_device_key`)
o `ed25519` (con la semilla de `sync_auth_seed`). Debe coincidir con `device_key_algorithm` en la
central. Un nodo sin el material de clave que su algoritmo necesita registra
`LORA_SIGNING_UNUSABLE` y no arranca el enlace; un nodo nunca provisionado envía tramas sin firma.

LoRa transmite solo mientras el Wi-Fi no está entregando (sin enlace, sin `sync_server_url` o
con la sincronización fallando), y saltea todo lo que el Wi-Fi ya entregó, así un nodo con ambos
usa la radio como respaldo sin gastar tiempo de aire dos veces.

Lo que no está probado: que el cableado sea correcto. `LORA_ENABLED` dice que el periférico SPI
arrancó y que la radio aceptó sus escrituras de configuración, no que el enlace funcione. El
gateway es un puente ESP32 + SX1276 (`pio run -e gateway`) conectado por USB al equipo de la
central, que alimenta a `cauce_server.gateway_runner`; ver DEPLOYMENT.md, sección 2c.

## Intercambio entre nodos (ESP-NOW)

Con `peer_enabled=1` los vecinos en el mismo `peer_channel` guardan copias de los registros de
los demás, así un nodo que se rompe no se lleva su historial. ESP-NOW no autentica nada, por lo
que cada trama va firmada con la semilla Ed25519 del emisor (`sync_auth_seed`) y solo se integra
si el emisor es de confianza y todos sus registros son propios. Un emisor es de confianza si está
en `peer_keys`, o si su anuncio trae una credencial firmada por la CA fijada en `ca_public_key`:
120 bytes que atan su id de nodo a su clave pública hasta un vencimiento, devueltos como
`peer_credential` cuando la central emite el certificado del nodo y cargados en la config del
nodo con ese mismo nombre. Volver a sembrar un nodo pasa a ser emitirle un certificado nuevo, no
editar a cada vecino; una credencial vencida deja de valer en cuanto el reloj es válido.
`tools/provision.py` sigue imprimiendo la línea `peer_keys` de un lote, y una clave listada
siempre gana sobre una avalada. Cada
ronda (`peer_interval_s`, 60 s) ofrece dos registros propios a cada vecino. Los registros de
vecinos quedan en LittleFS: el nodo que solo los guarda nunca los sincroniza ni los archiva.

## Tiempo

Jerarquía: **NTP > DS3231 > oscilador interno.** Con `rtc_enabled=1` el nodo toma la hora del
DS3231 al arrancar (salvo que su flag de oscilador detenido indique que se quedó sin pila, lo
que se registra como `RTC_TIME_UNTRUSTED`), y le escribe la hora NTP una vez que SNTP reporta
una sincronización completa. Los registros tomados antes de cualquier fuente confiable llevan
`TIME_UNCERTAIN` y se reparan en la central con `time-reconstruct`.

## Energía

La estación piloto funciona con un panel de 20 W, una batería de 12 V 7 Ah y un controlador de
carga de 10 A, con una etapa DC-DC para 5 V (SPS30, MH-Z19C) y 3,3 V. Son valores de partida,
no una autonomía demostrada: decide el balance energético de 14 días de `PILOT_SPEC.md`. El
divisor de batería (`battery_divider_ratio`, 100k/22k por defecto) alimenta `battery_voltage`,
cuyo rango válido por defecto cubre tanto una celda LiPo como una batería de 12 V.

## Lista de materiales (por nodo)

El documento de presupuesto del piloto lleva el BOM con precios (unos USD 255 por estación
completa); esta tabla lista los componentes y el motivo de cada elección.

| Subsistema | Componente | Por qué este |
|---|---|---|
| MCU | ESP32-WROOM-32, 4 MB | el target del firmware (`esp32dev`) |
| Radio | SX1276/RFM95W 915 MHz + antena | coincide con el driver existente |
| T/HR/P | BME280 | sensor de referencia, con driver desde v0 |
| PM | SPS30 | mide PM4 directamente |
| CO2 | MH-Z19C | NDIR, no una estimación |
| UV | VEML6075 | canales UVA y UVB separados |
| Luz / irradiancia | BH1750 + BPW34 | lux y W/m2 relativos son magnitudes distintas |
| Viento / lluvia | anemómetro, veleta, balancín | un kit, validado en un nodo antes de comprar siete |
| Suelo | sonda capacitiva, bloque de yeso, DS18B20 | humedad, succión y temperatura |
| Mojado foliar | sensor de rejilla | |
| Rayos | módulo AS3935 | |
| Reloj / archivo | DS3231, microSD 16 GB high-endurance | hora confiable sin red; historial CSV junto a LittleFS |
| Energía | panel 20 W, batería 12 V 7 Ah, controlador 10 A, DC-DC, fusible, TVS | |
| Mecánica | caja IP65, escudo de radiación impreso en PETG, mástil | mismo escudo y altura en todos los nodos |

## Almacenamiento

LittleFS en la flash interna es el almacenamiento operativo (unos 7.700 registros en 512 KiB) y la
fuente de toda sincronización. Con `sd_archive_enabled=1` la tarjeta es el historial:
`ArchiveManager` copia los registros de este nodo a `/archive/AAAA-MM-DD.csv` (días UTC; los
registros sin reloj confiable van a `/archive/untimed.csv`) y guarda su avance en
`/archive/watermark` dentro de la tarjeta. Una tarjeta ausente o fallando nunca frena la medición:
`/api/v1/health` muestra `archive_state: degraded`, el archivo reintenta con backoff y se pone al
día desde LittleFS cuando la tarjeta vuelve a funcionar. Los registros que la retención de LittleFS
borró antes de archivarse se cuentan en `archive_gap_records`. Las tarjetas van en FAT32.

## Cableado de referencia (ESP32 DevKit → BME280)

```
ESP32 3V3  → BME280 VCC     (¡chequear que el módulo sea realmente 3.3 V!)
ESP32 GND  → BME280 GND
ESP32 GPIO21 (SDA) → BME280 SDI
ESP32 GPIO22 (SCL) → BME280 SCK
BME280 SDO → GND            (dirección 0x76; a 3V3 = 0x77)
```

Los demás componentes I2C comparten los mismos dos cables; las direcciones no chocan.

## Consideraciones físicas (obligatorias para calidad científica)

- El sensor de T/HR **nunca** a sol directo ni pegado a un muro que irradia calor; 1,5–2,5 m
  de altura anda.
- La pantalla de radiación es obligatoria si querés que los nodos sean comparables siquiera.
  Misma pantalla, misma altura, o no compares.
- El SPS30 y el MH-Z19C necesitan flujo de aire pero no lluvia: van en el nivel inferior de la
  pantalla, con la entrada hacia abajo.
- Por nodo, anotar: fecha/hora de instalación, altura, orientación, desvío de norte de la
  veleta, profundidad de las sondas de suelo, superficie dominante del entorno (el contexto
  vive en `NodeConfig`).
- Sin certificado metrológico acá: estos números son para análisis comunitario comparativo, no
  meteorología oficial (ver CALIBRATION.md).
