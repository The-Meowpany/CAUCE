# Seguridad CAUCE — postura actual

Lo que realmente hacemos hoy, y lo que abiertamente no.

## Implementado

- **Tokens admin viven hasheados.** La config guarda el SHA-256 hex
  (`admin_token_sha256`) y nunca el plaintext. Nuestro SHA-256 está
  chequeado contra vectores NIST, y las comparaciones corren en tiempo
  constante en nodo y servidor — los timing attacks no sacan nada.
- **Validación de config con dientes.** Rangos numéricos, buffers de
  tamaño fijo, rechazo de basura. Pasale varias líneas malformadas y el
  parseo falla en vez de adivinar.
- **Sin heap en hot paths.** Medición, storage y config usan buffers
  fijos, lo que deja la superficie de overflow y fragmentación en casi
  nada.
- **Límites de tamaño como diseño.** Payload fijo de 60 bytes por
  record, segmentos acotados, retención con presupuesto en bytes. No hay
  lugar para una sorpresa de input gigante.
- **Hardening del backend.** Comparaciones en tiempo constante en todas
  partes (`hmac.compare_digest`), rate limiting por IP con memoria
  acotada en cada endpoint, tokens opcionales separados por scope (sync
  vs API).
- **Identidad por dispositivo y firma de lotes.** `/v1/provision`
  registra una key HMAC por nodo; los nodos provisionados firman cada
  lote de sync sobre el cuerpo crudo (`X-CAUCE-Signature`), computado
  con el mismo HMAC-SHA-256 verificado contra vectores RFC 4231. Una key
  robada te da un dispositivo, no la flota.
- **Gate del manifiesto OTA.** Con manifest key configurada, los
  releases sin HMAC válido sobre `version|url|totalSize` mueren antes de
  cualquier descarga — sin MITM de sustitución de manifiesto.
- **Split lectura/escritura.** Lecturas públicas, escrituras
  autenticadas, en la API del nodo y en el servidor central por igual.

## Pendiente (fases posteriores / hardware)

- Integración de radio Wi-Fi: las credenciales entran por la UI local, y
  el portal de provisioning se endurece junto con ella.
- Encriptación de transporte (TLS): la capa de decisión ya acepta URLs
  https vía `ISyncTransport` — lo que falta es la infraestructura de
  certificados y provisioning, no el camino de código.
- Flasheo OTA: la capa de decisión está hecha; la firma de payloads de
  release debería llegar antes de cualquier rollout a flota.
- Throttling del lado del nodo: el transporte hoy asume aislamiento LAN.
  Revisarlo el día que esto salga de la red piloto.

## No-negociables, ya aplicados

1. Cero secretos en plaintext en flash. Nunca.
2. Ningún input externo sin validar llega a la lógica.
3. Fail closed: un componente malo recibe rechazo explícito más línea
   de log, no un pase silencioso.
4. Privacidad: solo telemetría ambiental e ids de nodo. Sin datos
   personales, sin MACs de usuarios, nada identificatorio en URLs más
   allá de ids y timestamps.
