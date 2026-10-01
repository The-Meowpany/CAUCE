# Calibración CAUCE

## Modelo (definido; aplicación runtime pendiente)

```
calibrated_value = raw_value × scale + offset
```

Una línea de matemática, deliberadamente aburrida. Metadata por sensor
a registrar durante la fase de calibración:

- sensor_id, model, serial
- installation_date, calibration_date
- calibration_method, calibration_reference
- calibration_status

## Lo que el sistema afirma y lo que NO

**NO afirma:**

- Exactitud meteorológica profesional ni trazabilidad metrológica.
- Precisión de fábrica verificada experimentalmente por este proyecto —
  los números del datasheet están impresos en la tabla de abajo, no
  avalados.

**Sí soporta hoy:**

- **Comparación relativa entre nodos** sentados en el mismo contexto.
  "El nodo A lee 1.2 °C sobre el nodo B toda la semana" es una
  afirmación sólida; "afuera hay 24.7 °C" no es una que hagamos.
- Detección de anomalías de comportamiento (sensores congelados, que
  saltan, fuera de rango) vía el pipeline de validación.

## Distinciones obligatorias al reportar datos

| Término | BME280 (datasheet, no verificado por nosotros) |
|---|---|
| Resolución | 0.01 °C / 0.01 %RH |
| Exactitud del sensor | ±0.5 °C / ±3 %RH |
| Exactitud del sistema | **Aún no medida** — enclosure, self-heating y exposición cuentan y ninguno está caracterizado |
| Incertidumbre de calibración | **Aún no existe** — no hay proceso formal |

Regla del proyecto: cualquier afirmación científica que necesite
validación externa queda marcada **pendiente**. No presentamos deseos
como resultados.

## Plan mínimo recomendado (una vez que existan ≥2 nodos físicos)

1. Co-localizar todos los nodos 48 h en el mismo punto → computar
   offsets relativos nodo-a-nodo.
2. Registrar los offsets como calibraciones
   `method=co-location-relative`.
3. Repetir cada cambio de estación o tras mantenimiento.
4. Loguear cada evento en maintenance_events (fase backend).
