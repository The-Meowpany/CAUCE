# Seguridad CAUCE — estado actual

## Implementado

- **Tokens administrativos hasheados**: la configuración almacena solo
  SHA-256 del token (`admin_token_sha256`), nunca el secreto en texto
  plano. SHA-256 propio verificado contra vectores NIST.
- **Validación estricta de configuración**: rangos numéricos, longitudes
  acotadas por buffers fijos, rechazo de entrada basura (parseConfig
  devuelve error ante líneas malformadas múltiples).
- **Sin heap en el hot path**: buffers fijos en medición/storage/config →
  superficie de fragmentación y desbordamiento mínima.
- **Límites de tamaño desde el diseño**: payload de registro fijo (60 B),
  segmentos acotados, retención con tope de bytes.
- **Separación futura lectura/administración** ya prevista en el modelo de
  config (token admin vs datos públicos).

## No implementado todavía (fases 9/10/17 del plan)

- HTTP API: cuando exista, endpoints de escritura exigirán token
  (comparación constante-tiempo contra el hash) y rate limiting.
- Wi-Fi: credenciales hoy via config local física; el provisioning seguro
  (portal con token) es fase del captive portal.
- OTA firmado: pendiente; entre tanto NO habilitar actualizaciones remotas.
- Sanitización de inputs HTTP: no aplica aún (no hay HTTP).

## Principios que ya se aplican y no se negociarán

1. Ningún secreto en texto plano en flash.
2. Ninguna entrada externa sin validar contra esquema.
3. Falla cerrada: componente inválido → rechazo explícito + log, no
   comportamiento indefinido.
4. Privacidad: solo datos ambientales y telemetría del propio nodo. No se
   recolectan identificadores personales ni MACs de usuarios.
