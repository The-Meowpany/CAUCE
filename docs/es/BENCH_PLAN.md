# Plan de validación física en banco

Cada test automatizado corre en host con dobles parados donde iría el
hardware. Este plan cierra ese gap antes de que algo pise campo. Rig
mínimo: **3–4 nodos**, una fuente controlable (relé + corte
programable), un AP Wi-Fi con traffic shaping, una cámara ambiental — o
al menos exposición controlada de temperatura/humedad — y un termómetro
de referencia al lado de los nodos.

## B1. Integridad de flash bajo cortes reales

- Llenar storage cerca del presupuesto (≥60 KiB entre segmentos).
- Disparar 200 cortes aleatorios durante ventanas activas de append.
- Tras cada boot: `GET /api/v1/health` → `corrupted_frames` tiene que
  matchear lo esperado, la última secuencia tiene que estar contigua
  (regla max+1), y cada record consultado tiene que pasar CRC
  implícitamente.
- Comparar la granularidad de corrupción observada contra el modelo de
  frames. Si la realidad discrepa del modelo, pierde el modelo.

## B2. Fidelidad del sensor vs modelo del datasheet

- 72 h de soak con al menos 10 °C de swing ambiente, sonda de
  referencia al lado.
- |nodo − referencia| tiene que quedar dentro de la banda de
  tolerancia del datasheet; loguear la curva de drift igual.
- Ciclo de condensación: los estados de error del driver tienen que
  emerger como `INVALID`/`MISSING`, no como basura de aspecto plausible.
  Un número wrong creído es peor que ningún número.

## B3. Resistencia de radio

- 14 días continuos en STA: contar reconexiones, mirar drift de RSSI y
  high-water de heap (`/api/v1/health`), anotar cada reboot
  inesperado.
- Drill de AP-fallback: matar el link upstream repetidamente. La FSM
  tiene que llegar a `AP_FALLBACK`, el dashboard tiene que responder
  vía DNS del portal, y el nodo tiene que encontrar el camino de vuelta
  a STA.

## B4. Sync bajo fallos reales de red

- Correr la matriz E2E de host de nuevo, esta vez contra la central
  viva con nodos físicos: pérdida ≥30 % (tc/netem), restarts del server
  mid-batch, archivo watermark borrado de la flash del dispositivo.
- Aceptación: cero duplicados en el SQLite central tras cada escenario
  (`rows == distinct`), acks monótonos, halt solo ante rechazo
  semántico. La misma vara que en host, sin descuento por hardware.

## B5. Caracterización energética

- Medir corriente promedio en tres modos: siempre-encendido (hoy),
  radio con duty-cycle, deep sleep entre ciclos (una vez implementado).
- Chequear las recomendaciones de `SleepPolicy` contra los números
  medidos y publicar la tabla de dimensionado solar/batería derivada de
  mediciones — no de esperanzas.

## Criterios de salida

Los cinco bloques corridos, resultados registrados en este repositorio
(plantilla `BENCH_RESULTS.md` a crear por corrida). Cualquier mismatch
entre banco y predicciones del modelo host se vuelve issue trackeado
antes de la expansión en campo. Sin banco en verde, sin campo.
