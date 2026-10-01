# Dashboard web local CAUCE

## Acceso

Unirse al Wi-Fi del nodo (o a la misma LAN en modo estación) y abrir:

```
http://<node-ip>/            (ej. http://192.168.4.1 en modo AP)
```

Con el captive portal corriendo, cualquier dominio que el teléfono
prueba se vuelve el dashboard — sin adivinar la IP.

## Idiomas

Switch ES/EN en la página (guardado en `localStorage`, primera
adivinanza según idioma del browser). Las etiquetas estáticas salen de
un diccionario de traducción; los valores de dominio quedan como
códigos neutrales, así `VALID` nunca se vuelve un argumento de
traducción.

## Qué muestra

| Sección | Contenido |
|---|---|
| Tarjetas principales | Última temperatura/humedad con unidad, badge de **calidad**, tiempo desde la última lectura, estado del reloj del nodo |
| Gráfica | Serie temporal en `<canvas>` hecho a mano (sin librerías que bajar), rangos 1h / 6h / 24h / 7d, toggle Temperatura ↔ HR |
| Export | Links directos CSV y JSON de las últimas 24h |
| Salud | El JSON completo de `/api/v1/health`, sin filtrar |
| Configuración | Form chico (intervalo + token) que arma el cuerpo KV para `POST /api/v1/config` |

## Decisiones de diseño

- **Cero dependencias externas.** Sin CDNs, sin fuentes remotas, sin
  frameworks — la página anda desde flash sin nada de internet
  (~13KB embebido). Un dashboard que necesita la nube para mostrarte un
  sensor a dos metros sería vergonzoso.
- Mobile-first: grilla adaptativa, tipografía del sistema, alto
  contraste para el sol.
- La calidad del dato está visible, no enterrada: cada tarjeta lleva su
  estado VALID/SUSPECT/INVALID/MISSING en color.
- Refresh escalonado para que la CPU chica sobreviva: tarjetas 30s,
  salud 60s, gráfica 120s.
- Errores a la vista: pill roja "sin datos"; los fallos de config
  imprimen la lista exacta de errores de la API en vez de un ceño
  genérico.

## Implementación

El HTML/CSS/JS vive como string C++ (`WebAssets.cpp`) y `ApiRouter` lo
sirve como stream paginado (`StreamKind::Html`) — la misma maquinaria
de chunks que export. Los tests fijan la integridad (empieza con
`<!DOCTYPE`, termina con `</html>`, longitud completa entregada).

Captive portal: `Esp32CaptivePortal` (DNS wildcard→IP del nodo).
Compilación verificada; el comportamiento real con teléfonos espera
hardware.
