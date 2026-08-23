# Calibración CAUCE

## Modelo (definido, aplicación en runtime pendiente)

```
valor_calibrado = valor_crudo × escala + offset
```

Metadata asociada por sensor (a persistir en fase de calibración):

- sensor_id, model, serial
- installation_date, calibration_date
- calibration_method, calibration_reference
- calibration_status

## Lo que el sistema afirma y lo que NO

**NO afirma:**

- Exactitud meteorológica profesional ni trazabilidad metrológica.
- Precisión absoluta de fábrica verificada experimentalmente.

**Sí soporta hoy:**

- Comparación **relativa entre nodos** co-instalados en el mismo contexto.
- Detección de anomalías de comportamiento (congelamiento, saltos,
  fuera de rango) mediante el pipeline de validación.

## Distinciones obligatorias al reportar datos

| Término | En BME280 (datasheet, no verificado por nosotros) |
|---|---|
| Resolución | 0.01 °C / 0.01 %RH |
| Exactitud de sensor | ±0.5 °C / ±3 %RH |
| Exactitud de sistema | **No medida aún** — incluye carcasa, autocalentamiento, exposición |
| Incertidumbre de calibración | **No existe aún** — no hay proceso formal |

Regla para el proyecto: cualquier afirmación científica que requiera
validación externa se marca como **pendiente**, no como resultado.

## Plan mínimo recomendado (cuando haya ≥2 nodos físicos)

1. Co-locar todos los nodos 48 h en el mismo punto → calcular offsets
   relativos nodo-a-nodo.
2. Registrar esos offsets como calibración `method=co-location-relative`.
3. Repetir cada cambio estacional o tras mantenimiento.
4. Documentar cada evento en maintenance_events (fase backend).
