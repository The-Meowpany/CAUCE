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
(irradiancia solar relativa) · PMS5003 (PM1, PM2.5, PM10) · MH-Z19C (CO₂
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
práctica OMM). 21 variables por estación.

El SPS30 y el VEML6075 cuestan de cuatro a seis veces más a cambio de PM4 y
de separar UVA y UVB; el firmware los sigue manejando (`pm_pms5003=0`,
`uv_ltr390=0`) para un despliegue que necesite esos datos.

Cada sensor tiene driver en el firmware y una clave de config que lo
habilita; el mapa de pines está en `HARDWARE.md`. Los drivers están
probados en host contra buses simulados y todavía no corrieron en placa.

## 5. Costo por estación

Menores precios encontrados el 21/11/2026: AliExpress con envío a Uruguay
para la electrónica, comercios locales para lo pesado o lo que localmente
es más barato (batería, panel, caño, caja, antena). Al tipo de cambio
vendedor del BROU de $U 41,95 más una previsión del 10 % por variación de
precios: USD 264,77 por estación, con el detalle en el documento de
presupuesto.

## 6. Presupuesto (USD 4.000 disponibles)

| Concepto | USD |
|---|---|
| 4 estaciones idénticas | 1.059,08 |
| Nodo central: gateway y servidor (Orange Pi Zero 3) | 79,70 |
| Herramientas y banco de pruebas | 132,03 |
| Calibración y co-localización | 30,00 |
| Programa educativo | 1.573,84 |
| Logística, operación e importación | 754,26 |
| Repuestos críticos | 49,94 |
| Contingencia de doble propósito | 270,00 |
| **Total del proyecto** | **3.948,85** |
| No utilizado de los USD 4.000 | 51,15 |

La contingencia se dimensiona por el riesgo de los precios. Hasta cerrar la
etapa 4 (todo comprado e importado) cubre lo que termine costando más de
lo previsto: precios por encima de la previsión, importación por encima de
la provisión, una antena para un sitio al que no llega el gateway,
componentes fallidos más allá de los repuestos. Lo que quede financia
entonces, antes de la co-localización, la calibración trazable del
instrumento de referencia del banco, propio del proyecto (o uno certificado
si ese no admite certificación), que es lo que convierte la
co-localización de consistencia relativa en sesgo absoluto.

El programa educativo concentra lo que la red ya no necesita: seis kits
de práctica (ESP32 y un juego de sensores) que quedan en las instituciones,
una impresora de tinta continua y una plastificadora para guías, etiquetas
y certificados, un roll-up, la merienda de los talleres y los boletos
suburbanos del equipo y de los estudiantes que viajan a la jornada de
cierre. La logística incluye además los boletos de cuatro rondas de
mantenimiento durante el piloto.

No hay impresora 3D: un escudo de radiación comercial cuesta menos que
imprimirlo. No hay bloques de yeso comerciales disponibles, así que se
fabrican en los talleres. Los tributos de importación se solicitan a la
Intendencia de Canelones y, mientras tanto, quedan reservados en el
presupuesto para que las compras no esperen la respuesta: con el régimen de
prestación única del Decreto 50/2026 (60 % del valor de cada envío, mínimo
USD 20, que reemplaza al IVA y a los demás tributos) suman USD 387,42 sobre
unos USD 639 de mercadería en 11 envíos, comprada en un solo pedido para
pagar el mínimo una sola vez por parte. Si la Intendencia los cubre, la
reserva paga la calibración trazable y dos talleres más. Sin línea de intervenciones
físicas: el proyecto genera evidencia para evaluar medidas, no las
ejecuta.

Lo que queda después: hardware, firmware, protocolo, datos, metodología
de calibración, plataforma.

## 7. Calibración

Co-localización de las 4 estaciones dos semanas antes del despliegue;
coeficientes `cal_*` individuales por nodo y variable; informe de
divergencias. Referencia primero: el termohigrómetro de referencia
propio del banco, con certificado de calibración de un laboratorio
acreditado pagado con el saldo de la contingencia; todo instrumento del
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
