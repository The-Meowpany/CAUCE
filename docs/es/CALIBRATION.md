# Calibración CAUCE

## Modelo

```
calibrated_value = raw_value × scale + offset
```

Implementado en el central, que es la **única fuente de verdad**.
`measurements.value` nunca se reescribe: el valor calibrado se deriva en la
lectura, así una recaliibración se puede volver a aplicar sobre todo el
histórico sin tocar un nodo ni perder el registro crudo.

Una línea de matemática, deliberadamente aburrida. Metadata por sensor a
registrar durante la fase de calibración:

- sensor_id, model, serial
- installation_date, calibration_date
- calibration_method, calibration_reference
- calibration_status

## Por qué en el central y no en el nodo

Un offset `co-location-relative` solo existe después de que varios nodos
estuvieron en el mismo lugar juntos, así que el conocimiento nace en el
centro. Mandarlo de vuelta a los nodos duplicaría un estado que después
puede divergir, y el nodo terminaría guardando un número que no puede
verificar. Por eso el nodo sigue reportando crudo y el central hace la
aritmética.

## API

| Método | Ruta | Descripción |
|---|---|---|
| PUT | `/v1/sites/{id}/calibration` | Upsert de un registro `(sitio, variable)`. Token admin |
| GET | `/v1/sites/{id}/calibration` | Todos los registros del sitio, con el modelo explícito |
| POST | `/v1/sites/{id}/maintenance` | Registrar un evento de mantenimiento (instalación, calibración, recambio de sensor, traslado) |
| GET | `/v1/sites/{id}/maintenance` | Bitácora de mantenimiento, de más nuevo a más viejo |

`scale` tiene que ser finito y distinto de cero; `offset` tiene que ser
finito. Ambos se validan por rango para cazar errores de tipeo, no para
 policingar valores legítimos. `status` es uno de `applied`,
`provisional`, `retired`, `rejected`, y `retired` saca el registro de
todos los cálculos sin borrarlo.

## Dónde se aplica

| Superficie | Comportamiento |
|---|---|
| `/v1/analytics/summary`, `/compare`, `/period-compare` | Los números de arriba siguen crudos; aparece un bloque `calibrated` al lado cuando hay registro |
| `/v1/analytics/summary-fast`, `granularity=hourly` | Igual, aplicado sobre los agregados horarios |
| `/v1/analytics/heat-events` | Los eventos se detectan sobre valores calibrados, así un umbral significa lo que el operador espera |
| `/v1/analytics/before-after` | `mean_shift_calibrated` y `difference_in_differences_calibrated` junto a las cifras crudas |
| `/colocation` | Se aplica antes de calcular el sesgo, que es lo que mide el criterio de aceptación del piloto |
| `/nodes/{id}/report` | Columnas extra de min/max/media calibradas, `*` marca variable calibrada |
| Exports CSV | Tres columnas agregadas al final: `calibrated_value`, `calibration_scale`, `calibration_offset` |

Como el mapa es lineal, la transformación es exacta sobre agregados: las
estadísticas de ubicación se trasladan, la dispersión escala por
`|scale|`, y no hace falta releer las filas crudas para corregir un bucket
horario.

 Toda respuesta trae un objeto `calibration` indicando si se aplicó y con
qué escala, offset, método y fecha. Cuando no se aplicó, la clave
`calibrated` está ausente en vez de ser idéntica al valor crudo.

## Lo que el sistema afirma y lo que NO

**NO afirma:**

- Precisión meteorológica profesional ni trazabilidad metrológica.
- Precisión de fábrica verificada experimentalmente por este proyecto:
  los números del datasheet están en la tabla de abajo, no respaldados.
- Que un nodo calibrado sea *correcto*: un offset de co-localización
  arregla la diferencia entre dos sensores en un lugar y un momento, y
  nada más.

**Sí soporta hoy:**

- **Comparación relativa entre nodos** en el mismo contexto. "El nodo A
  lee 1.2 °C más que el B toda la semana" es una afirmación sólida; "están
  a 24.7 °C afuera" no es una afirmación que hagamos.
- Detección de anomalías de comportamiento (sensor congelado, saltando,
  fuera de rango) por el pipeline de validación.

## Distinciones obligatorias al reportar datos

| Término | BME280 (datasheet, no verificado por nosotros) |
|---|---|
| Resolución | 0.01 °C / 0.01 %RH |
| Exactitud del sensor | ±0.5 °C / ±3 %RH |
| Exactitud del sistema | **Sin medir todavía** — la caja, el autocalentamiento y la exposición cuentan y ninguno está caracterizado |
| Incertidumbre de calibración | **No existe todavía** — no hay proceso formal |

Regla del proyecto: cualquier afirmación científica que necesite
validación externa queda marcada como **pendiente**. No presentamos
deseos como resultados. Un registro con `method=co-location-relative` y
sin estimación de incertidumbre va en un informe bajo "pendiente", no bajo
"medido".

## Incertidumbre de calibración

Un registro de calibración puede llevar una `uncertainty` absoluta y el
`uncertainty_kind` que dice de dónde salió: `sensor_datasheet`,
`co_location_spread`, `repeatability`, `estimated` o `unknown`.

Dos reglas le dan valor:

- **`null` no es cero.** Una incertidumbre ausente significa que nadie la
  caracterizó, que es una afirmación distinta de "es exacto". El CSV deja la
  columna vacía en ese caso, y toda respuesta de analítica reporta
  `uncertainty: null`.
- **Escala con la corrección.** Una escala de 0.5 reduce a la mitad la
  incertidumbre que introduce la escala, así que la cifra reportada es la
  incertidumbre *después* de calibrar. Una escala negativa usa su magnitud,
  porque una reflexión no agrega error.

Un `uncertainty_kind` sin `uncertainty` da `422`: nombrar el tipo de una
incertidumbre que nadie cuantificó es peor que no decir nada. Una actualización
parcial que omita ambos conserva lo anterior, así que re-postear un offset no
borra en silencio una caracterización que costó una semana de co-localización.

Lo que esto no hace es volver trazable al proyecto. Permite que un informe diga
cuánto de un número es medición y cuánto es método. Sigue sin haber un
procedimiento de calibración ni una afirmación metrológica en el sistema.


## Plan mínimo recomendado (una vez que existan ≥2 nodos físicos)

1. Co-localizar todos los nodos 48 h en el mismo lugar → calcular los
   offsets relativos nodo a nodo.
2. Registrar los offsets como calibraciones
   `method=co-location-relative` y anotar el evento en
   `POST /v1/sites/{id}/maintenance`.
3. Volver a `/colocation` y leer el spread post-calibración.
4. Repetir en cada cambio de estación o después de mantenimiento:
   `status=retired` el registro anterior en vez de borrarlo.
5. Verificar que el criterio de aceptación del piloto (spread ≤0.2 °C /
   ≤3 %RH) se evalúa sobre la columna calibrada, y citar el spread crudo
   al lado.
