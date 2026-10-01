# Glosario de dominio CAUCE

Un concepto, un término. Este archivo existe porque los primeros
borradores usaban tres palabras inglesas distintas para lo mismo según
quién escribía el párrafo. Cuando el español mapeaba históricamente a
varios candidatos en inglés, el ganador figura acá y se usa en todas
partes — código, esquemas, logs, tests, docs.

## Núcleo

| Término | Definición |
|---|---|
| **Node** | Un dispositivo microestación (`CAUCE-001`…). Id canónico: `node_id`. Si hablás de la caja física, decí node. Si hablás del lugar, eso es un site. |
| **Site** | Donde se instalan uno o más nodos (`site_id`). Lleva metadata de emplazamiento: latitud, longitud, elevación, land cover, shade condition. |
| **Measurement** | La unidad atómica de dato: `node_id + sequence`, timestamp UTC, variable, valor, calidad, reason bits. Nunca se borra; los valores malos se guardan marcados, no se tapan. |
| **Variable** | Cantidad ambiental enumerada: `air_temperature`, `relative_humidity`, `pressure`, `illuminance`, `battery_voltage`. Conjunto cerrado — agregar una toca firmware, backend y docs. |
| **Quality** | Máquina de estados de evaluación: `VALID`, `CALIBRATED`, `UNCALIBRATED`, `ESTIMATED`, `SUSPECT`, `INVALID`, `MISSING`. La asigna el validador, nunca a mano. |
| **Reason bits** | Bitmask que dice *por qué* se asignó una calidad (rango, no-finito, tasa-de-cambio, stuck, tiempo-incierto, duplicado). El recibo, no solo el veredicto. |

## Almacenamiento

| Término | Definición |
|---|---|
| **Frame** | Record de 68 bytes en flash: magic + versión + longitud + payload + CRC32. |
| **Segment** | Archivo `.clog` append-only de frames. Se sella cuando su cola se corrompe; las escrituras nuevas rotan a un segmento fresco. Los segmentos viejos mueren por retención. |
| **Watermark** | Máxima `sequence` con ack para un nodo, persistida por el cliente de sync tras cada ack. Perderlo y reenviar desde cero es por diseño, no por accidente. |
| **Retention policy** | Regla que borra los segmentos más viejos pasado un presupuesto en bytes. Al menos un segmento sobrevive siempre, aunque exceda el presupuesto. |
| **Integrity check** | Re-escaneo completo contando segmentos corruptos/inparseables. Un número, no una sensación. |

## Red y sync

| Término | Definición |
|---|---|
| **Sync batch** | Payload JSON con hasta N mediciones desde el watermark. `batch_size` siempre iguala los records realmente serializados — conteos inflados corromperían la contabilidad del servidor. |
| **Acknowledged sequence** | Máxima `sequence` que el servidor confirma persistida. Nunca fabricada; la suite E2E la verifica contra SQLite. |
| **Idempotent replay** | Reenvío de records con ack tras pérdida del watermark. Tráfico esperado, deduplicado en `(node_id, sequence)`. |
| **Degraded** | Estado de link: RSSI bajo umbral, tráfico fluyendo igual. El sync sigue — degraded no es down. |
| **AP fallback** | Access point del lado del nodo tras fallos STA repetidos. Hospeda el captive portal para que un humano igual llegue al nodo. |

## Analítica

| Término | Definición |
|---|---|
| **Derived metric** | Cualquier estadístico computado (media, percentil, horas de exposición). Con tag `metric_type: derived`, nunca presentado como medición cruda. Una media es una opinión con matemática. |
| **Before/after evaluation** | Comparación partida por la ventana temporal de una intervención. Bajo 30 muestras válidas por período se marca `sufficient_sample: false` y se deja de interpretar. |
| **Exposure hours** | Duración trapezoidal sobre un umbral de temperatura entre muestras consecutivas. |
| **Causality disclaimer** | Nota obligatoria: las diferencias suelen significar emplazamiento o calibración, no prueba de nada. |

## Updates y energía

| Término | Definición |
|---|---|
| **OTA release** | Entrada de manifiesto `{version, sha256_hex, url, total_size}`. |
| **Reboot pending** | Imagen nueva verificada y staged; el dispositivo reinicia en ella. El rollback usa el patrón boot-counter de partición alterna (bring-up de hardware pendiente). |
| **Sleep advisory** | Una recomendación de `SleepPolicy`. Advisory significa advisory — nunca aplicada automáticamente. |

## Presentación pública

| Término | Definición |
|---|---|
| **Locale labels** | Los strings humanos viven solo en diccionarios de presentación (`I18N` en la SPA del nodo, `LABELS` en el dashboard central). Los valores de dominio quedan como códigos neutrales (`VALID`, no "Válido") — traducir un código empieza discusiones, no conversaciones. |
