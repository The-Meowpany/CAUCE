# Dashboard web local — CAUCE

## Acceso

Al conectarse al Wi-Fi del nodo (o a la misma red si el nodo está en modo
station), abrir:

```
http://<ip-del-nodo>/            (ej. http://192.168.4.1 en modo AP)
```

Con portal cautivo activo, cualquier dominio que el teléfono intente
resolver redirige al dashboard automáticamente.

## Qué muestra

| Sección | Contenido |
|---|---|
| Tarjetas principales | Última temperatura/humedad con unidad, **calidad** de la medición, hace cuánto fue la última lectura, estado temporal del nodo |
| Gráfico | Serie temporal dibujada en `<canvas>` nativo (sin librerías), rangos 1h / 6h / 24h / 7d, alternancia Temperatura ↔ Humedad relativa |
| Exportación | Enlaces directos CSV y JSON de las últimas 24 h (`/api/v1/export`) |
| Salud | JSON de `/api/v1/health` con contadores completos |
| Configuración | Formato mínimo (intervalo + token) que arma el KV y hace `POST /api/v1/config` |

## Decisiones de diseño

- **Cero dependencias externas**: sin CDN, fuentes remotas ni frameworks —
  funciona 100% offline desde la flash del nodo (~9.5 KB embebidos).
- Mobile-first: grid autoajustable, tipografía sistema, contraste alto.
- La calidad de datos es ciudadano de primera clase: cada tarjeta muestra
  el estado VALID/SUSPECT/INVALID/MISSING con color.
- Refresco automático: tarjetas 30 s, salud 60 s, gráfico 2 min.
- Errores visibles: sin datos → pill "sin datos" rojo; fallo de config →
  mensaje exacto devuelto por la API.

## Implementación

- HTML/CSS/JS embebidos como string C++ (`WebAssets.cpp`), servidos por
  `ApiRouter` como stream paginado (`StreamKind::Html`) — misma mecánica
  de chunks que export; verificado con tests de integridad
  (inicio `<!DOCTYPE`, cierre `</html>`, tamaño total).
- Portal cautivo: `Esp32CaptivePortal` (DNSServer wildcard→IP del nodo);
  compilación verificada, comportamiento real pendiente de placa.
