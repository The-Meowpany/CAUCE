# CAUCE — Especificación congelada del piloto

Anexo técnico. La solicitud del fondo lleva únicamente Meta y Resultado
(sección 1); todo lo demás es especificación para construir.

## 1. Formulario (versión evaluador)

**Meta:** desarrollar y validar una red piloto de ocho microestaciones
ambientales autónomas capaces de generar información hiperlocal y
comparable sobre condiciones microclimáticas en Canelones.

**Resultado:** ocho estaciones calibradas y operativas, generando
registros ambientales durante el período piloto.

## 2. Alcance congelado

8 nodos idénticos · 1 gateway · 1 carrier PCB CAUCE-Node · 1 protocolo
de datos · 1 esquema de calibración · 1 sistema de almacenamiento local ·
1 sistema LoRaWAN · 1 API central · 1 visor microclimático.

Regla de oro: **el nodo nunca depende del gateway para medir.**

Regla de compra: **ningún componente crítico se adquiere ×8 hasta que
el prototipo supere la aceptación** (esto es sobre todo por el
anemómetro: se compra uno, se valida, recién después los otros siete).

Cadena causal del proyecto:
**medir → validar → calibrar → conservar → transmitir → comparar →
visualizar → generar evidencia.** Ninguna intervención física se
compromete con estos fondos.

## 3. Arquitectura congelada

```text
                    ┌───────────────┐
                    │   Sensores    │
                    └───────┬───────┘
                            ↓
                       ┌─────────┐
                       │  ESP32  │
                       └────┬────┘
                            │
               ┌────────────┴────────────┐
               ↓                         ↓
          LittleFS                    microSD
       buffer operativo            archivo histórico
               │                         │
               └────────────┬────────────┘
                            ↓
                          LoRa
                            ↓
                         Gateway
                            ↓
                       ChirpStack
                            ↓
                           API
                            ↓
                 Motor de procesamiento
                            ↓
                Mapa microclimático 2D esquemático
                 + incertidumbre
```

LittleFS es el buffer operativo; microSD es el archivo histórico. Si la
SD falla (montaje, escritura, corrupción), el nodo sigue corriendo sobre
LittleFS y levanta un flag de health: ninguna falla de almacenamiento
detiene jamás la medición. El gateway agrega; no es la fuente de verdad —
el nodo conserva siempre sus datos y el servidor consolida.

Lenguaje del proyecto: "mapa microclimático (2D esquemático con campo
interpolado e incertidumbre explícita)". Viento: "velocidad y
dirección por estación → campo vectorial interpolado". El render
volumétrico 3D está descartado por decisión de diseño. Cualquier cosa
más grandilocuente tiene que ganarse la redacción con densidad de nodos
primero.

## 4. Nodo (×8 idénticos)

Heltec WiFi LoRa 32 V3 (868 MHz) · SHT35 (temperatura + humedad de
referencia) · BME280 (presión) · SCD40 (CO₂, capa ambiental
complementaria, calibración forzada FRC en co-localización) · BH1750
(iluminancia ambiental — eso es lux, no irradiancia solar) ·
anemómetro ultrasónico RS485 candidato, pendiente de validación ·
pluviómetro de balancín · 18650 con protección · panel 5 V + manager de
carga · carrier PCB CAUCE-Node (potencia, I²C, RS485 con TVS SM712 y
terminación, SPI/SD, DS3231, corte de sensores por GPIO, protecciones) ·
microSD industrial high-endurance 16 GB en FAT32 · DS3231 (±2 ppm;
jerarquía NTP > LoRaWAN > DS3231 > interno) · caja IP65 con
prensaestopas · antena 868 MHz + pigtail · pantalla Stevenson impresa +
mástil (protocolo de instalación a 2 m, práctica OMM).

## 5. BOM prototipo ×1 (~USD 280)

Heltec V3 20 · SHT35 + BME280 + BH1750 18 · SCD40 del mismo proveedor de
la tanda final 45 · anemómetro ultrasónico RS485 candidato, pendiente de
validación 60 · pluviómetro 18 · 18650 + panel + manager 23 · carrier
(tanda prototipo, prorrateo) 40 · SD + RTC + RS485 + protecciones + caja
+ antena + mástil 56.

## 6. Presupuesto (USD 4.000)

| Concepto | USD |
|---|---|
| Construcción de 8 estaciones: hasta | 1.920 |
| Gateway e infraestructura LoRa | 200 |
| Repuestos | 350 |
| Protección e instalación | 300 |
| Fabricación de carrier PCB | 150 |
| Transporte y logística | 250 |
| Comunicación y señalización | 200 |
| Capacitación y participación | 200 |
| Contingencia | 430 |
| **Total** | **4.000** |

"Hasta USD 1.920" es un techo con margen para deriva de precios,
importación y sustitución por equivalentes (BOM de §4–5 como anexo:
USD 235 + USD 5 de margen de fabricación por nodo). Sin línea de
intervenciones físicas: el proyecto genera evidencia para evaluar
medidas, no las ejecuta.

Lo que queda después: hardware, firmware, protocolo, datos, metodología
de calibración, plataforma.

## 7. Calibración

Co-localización de los 8 nodos dos semanas antes del despliegue;
coeficientes `cal_*` individuales por nodo y variable; informe de
divergencias. Referencia primero: intentar un instrumento patrón
prestado (universidad / organismo). Si falla, intercomparación entre
los ocho: **eso establece consistencia relativa, no sesgo absoluto**,
y los documentos científicos tienen que decirlo (un sesgo compartido
por todos los nodos sería invisible). Re-validación periódica en campo
por comparación entre vecinos; divergencia sostenida = flag de
mantenimiento, no dato silencioso.

## 8. Aceptación del prototipo (pasa / no pasa)

| Prueba | Vara |
|---|---|
| Balance energético 14 días | Balance diario positivo medido + reserva registrada (hipótesis de diseño ~160 mAh/día; la autonomía se valida, no se promete) |
| Viento 14 días co-localizado | Sesgo velocidad ≤10 %, dirección ±≤5 °, sin caídas con lluvia; si no, plan B mecánico |
| Lluvia multi-tasa | Varios volúmenes y tasas simuladas (fina, media, intensa); conteos dentro de ±10 % en cada una |
| Extracción de SD en caliente | Sigue midiendo sobre LittleFS, flag en health, cero resets |
| Gateway muerto 48 h | Cero pérdidas al reanudar (watermark + SD + time-reconstruct) |
| Co-localización T/HR | Dispersión post-`cal_*` ≤±0,2 °C / ±3 %HR |

## 9. Gestión de energía y fallos

Dimensionado para operación autónoma con reserva; validado
experimentalmente en campo, no en papel. El anemómetro (tensión de
alimentación y consumo con boost) es la incógnita declarada del
budget. Fallos cubiertos: SD, gateway, alimentación intermitente,
deriva de reloj (DS3231 + corrección posterior en servidor), sensor
congelado/desconectado (ya manejados por el motor de validación del
firmware).
