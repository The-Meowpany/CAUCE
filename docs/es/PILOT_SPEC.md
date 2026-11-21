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
1 enlace LoRa punto a punto · 1 API central · 1 visor microclimático.

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
               lora_gateway → POST /v1/sync
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

ESP32-WROOM-32 + SX1276/RFM95W a 915 MHz (AU915, la banda de Uruguay) ·
BME280 (temperatura, humedad, presión) · BH1750 (iluminancia — lux, no
irradiancia solar) · fotodiodo BPW34 con etapa de transimpedancia
(irradiancia solar relativa) · SPS30 (PM1, PM2.5, PM4, PM10) · MH-Z19C
(CO₂ NDIR, corrección automática de línea base apagada, cero en
co-localización) · VEML6075 (índice UV con UVA y UVB separados) ·
AS3935 (conteo y distancia de descargas) · anemómetro de cazoletas +
veleta de resistencias + pluviómetro de balancín (un kit validado antes
de comprar siete) · sonda capacitiva de humedad de suelo, bloque de yeso
(potencial hídrico) y DS18B20 (temperatura de suelo) · sensor de mojado
foliar · DS3231 (±2 ppm; jerarquía NTP > DS3231 > interno) · microSD
16 GB high-endurance (archivo CSV) · panel 20 W, batería
12 V 7 Ah, controlador 10 A, DC-DC, fusible, TVS · caja IP65 con
prensaestopas · antena 915 MHz + pigtail · pantalla de radiación impresa
en PETG + mástil (protocolo de instalación a 2 m, práctica OMM).

Cada sensor tiene driver en el firmware y una clave de config que lo
habilita; el mapa de pines está en `HARDWARE.md`. Los drivers están
probados en host contra buses simulados y todavía no corrieron en placa.

## 5. BOM prototipo ×1 (~USD 255)

ESP32 3 · SX1276 + antena 7 · BME280 7 · SPS30 36,12 · MH-Z19C 24,99 ·
VEML6075 6 · BH1750 1,50 · etapa BPW34 3 · kit viento/lluvia 34 · sondas
de suelo 9 · mojado foliar 3 · AS3935 12 · DS3231 3 · microSD 6 · panel
28 · batería 27 · controlador de carga 7,64 · regulación y protección 5 ·
caja 7,39 · pantalla impresa 4 · PCB y conectores 12 · mástil 8.
Total USD 254,64.

## 6. Presupuesto (USD 4.000)

| Concepto | USD |
|---|---|
| 8 estaciones completas | 2.037,12 |
| Gateway LoRa punto a punto (ESP32 + SX1276) | 60,00 |
| Servidor central | 90,00 |
| Herramientas, fabricación y banco de pruebas | 603,00 |
| Calibración y co-localización | 30,00 |
| Talleres y jornadas educativas | 480,00 |
| Instalación y logística local | 140,00 |
| Transporte internacional e importación (provisión) | 400,00 |
| Repuestos críticos | 78,11 |
| Contingencia | 81,77 |
| **Total** | **4.000,00** |

La provisión de importación cubre un costo efectivo de hasta ~30 % sobre
unos USD 1.356 de mercadería importada; el documento de presupuesto
lista las palancas de ajuste si la cifra real es mayor. Sin línea de
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
experimentalmente en campo, no en papel. El ventilador del SPS30 y la
lámpara del MH-Z19C (ambos a 5 V) son las incógnitas declaradas del
balance energético; el divisor de batería reporta `battery_voltage`
para que el balance se mida en lugar de suponerse. Fallos cubiertos: SD, gateway, alimentación intermitente,
deriva de reloj (DS3231 + corrección posterior en servidor), sensor
congelado/desconectado (ya manejados por el motor de validación del
firmware).
