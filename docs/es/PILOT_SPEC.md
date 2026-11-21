# CAUCE — Especificación congelada del piloto

Anexo técnico. La solicitud del fondo lleva únicamente Meta y Resultado
(sección 1); todo lo demás es especificación para construir.

## 1. Formulario (versión evaluador)

**Meta:** desarrollar y validar una red piloto de cuatro microestaciones
ambientales autónomas capaces de generar información hiperlocal y
comparable sobre condiciones microclimáticas en Canelones.

**Resultado:** cuatro estaciones calibradas y operativas, generando
registros ambientales durante el período piloto.

## 2. Alcance congelado

5 nodos: 4 estaciones idénticas + 1 nodo central (gateway y servidor) · 1 carrier PCB CAUCE-Node · 1 protocolo
de datos · 1 esquema de calibración · 1 sistema de almacenamiento local ·
1 enlace LoRa punto a punto · 1 API central · 1 visor microclimático.

Regla de oro: **el nodo nunca depende del gateway para medir.**

Regla de compra: **ningún componente se compra en cantidad hasta que
el prototipo supere la aceptación**: el prototipo es una estación
entera, así que cada tipo de sensor se valida antes del lote.

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

## 4. Estación (×4 idénticas)

ESP32-WROOM-32 + RFM95W (SX1276) a 915 MHz (AU915, la banda de Uruguay) ·
BME280 (temperatura, humedad, presión) · BH1750 (iluminancia — lux, no
irradiancia solar) · fotodiodo BPW34 con etapa de transimpedancia
(irradiancia solar relativa) · SPS30 (PM1, PM2.5, PM4, PM10) · MH-Z19C (CO₂
NDIR, corrección automática de línea base desactivada, cero en
co-localización) · LTR390 (índice UV) · AS3935 (conteo y distancia de
descargas) · anemómetro de cazoletas, veleta y pluviómetro de balancín
Misol · sonda capacitiva de humedad de suelo, bloque de yeso artesanal
(potencial hídrico, leído con polaridad alterna mediante un expansor
MCP23017) y DS18B20 (temperatura de suelo) · placa de mojado foliar ·
DS3231 (±2 ppm; jerarquía NTP > DS3231 > interno) · microSD 32 GB (archivo
CSV) · panel 20 W, batería 12 V 7 Ah, controlador de carga PWM, DC-DC,
fusible, TVS · caja estanca con prensaestopas · antena 915 MHz · escudo de
radiación comercial de ABS + mástil (protocolo de instalación a 2 m,
práctica OMM). 22 variables por estación. La primera estación armada es
el prototipo que tiene que superar la sección 8.

El LTR390 reemplaza al VEML6075, que Vishay discontinuó: da el índice UV
sin separar UVA y UVB (`uv_ltr390=1`). El PMS5003 es la alternativa al
SPS30 (`pm_pms5003=1`, pierde PM4) si los precios del equipamiento llevan
el presupuesto por encima del tope del 50 % de las bases.

Cada sensor tiene driver en el firmware y una clave de config que lo
habilita; el mapa de pines está en `HARDWARE.md`. Los drivers están
probados en host contra buses simulados y todavía no corrieron en placa.

## 5. Costo por estación

Menores precios encontrados el 21/11/2026: AliExpress con envío a Uruguay
para la electrónica, comercios locales para lo pesado o lo que localmente
es más barato (batería, panel, caño, caja, antena), al tipo de cambio
vendedor del BROU de $U 41,95: USD 291,70 por estación, con el detalle en
el documento de presupuesto.

## 6. Presupuesto (USD 4.000, Fondo Juventudes en Acción Climática 2026-2027)

| Concepto | USD |
|---|---|
| 4 estaciones idénticas | 1.166,80 |
| Nodo central: gateway y servidor (Orange Pi Zero 3) | 72,45 |
| Herramientas e instrumental de prueba, placas CAUCE-Node | 181,98 |
| Importación del equipamiento (prestación única y entrega) | 539,13 |
| Calibración: certificado del LATU, materiales de instalación | 309,49 |
| Talleres e impresión de materiales | 309,20 |
| Transporte y viáticos del equipo | 814,38 |
| Gastos administrativos de la organización patrocinadora (5 %) | 200,00 |
| Imprevistos | 406,57 |
| **Total del proyecto** | **4.000,00** |

Las bases limitan el equipamiento y la infraestructura al 50 % del
proyecto y los gastos administrativos al 15 %. El equipamiento, contado
con su importación, suma USD 1.960,36 (49,0 %); los tributos de
importación (USD 479,13 por el Decreto 50/2026, 60 % de cada envío, mínimo
USD 20) están dentro de los USD 4.000. Los talleres son abiertos y
gratuitos; a quienes participan se los invita a sumarse al proyecto y a su
organización en GitHub, y reciben un certificado y stickers del proyecto.
La ejecución termina el 15/04/2027 y el informe final se presenta el
22/04/2027. No hay impresora 3D: un escudo de radiación comercial cuesta
menos que imprimirlo. Los bloques de yeso se fabrican en el proyecto. Sin
línea de intervenciones físicas: el proyecto genera evidencia para evaluar
medidas, no las ejecuta.

Lo que queda después: hardware, firmware, protocolo, datos, metodología
de calibración, plataforma.

## 7. Calibración

Co-localización de las 4 estaciones dos semanas antes del despliegue;
coeficientes `cal_*` individuales por nodo y variable; informe de
divergencias. Referencia primero: el termohigrómetro de referencia
propio del banco, con certificado de calibración de un laboratorio
acreditado (LATU), incluido en el presupuesto; todo instrumento del
plan es propio, ninguno prestado ni compartido. Hasta tener ese
certificado, la intercomparación entre las cuatro estaciones **establece consistencia
relativa, no sesgo absoluto**, y los documentos científicos tienen que
decirlo (un sesgo compartido por todos los nodos sería invisible). Re-validación periódica en campo
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
