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

## Retirar un dispositivo

Un dispositivo perdido, robado o sospechoso de estar comprometido puede tener su
identidad retirada sin que nadie tenga que llegar hasta él:

```
POST /v1/nodes/{node_id}/revoke      {"reason": "..."}      -> scope admin
GET  /v1/nodes/{node_id}/revocation
```

Tres propiedades, cada una una decisión y no un detalle de implementación:

- **Retirar no es borrar.** Las mediciones que el nodo ya aportó se quedan. Un
  dispositivo comprometido durante una semana sí hizo observaciones reales durante
  parte de esa semana, y borrar la fila se lleva por delante la evidencia.
- **No hay "des-retirar".** Reinscribir significa aprovisionar de nuevo con una clave
  **nueva**, y eso es lo que limpia el retiro. Un endpoint que solo devolviera un flag
  restauraría una identidad sin cambiar aquello que quedó comprometido. Se afirman
  como 404 tres grafías plausibles de esa ruta.
- **La comprobación va antes de la autenticación.** Comprobar después de una firma
  válida gastaría el presupuesto de rate limit del central verificando frames de un
  dispositivo ya retirado.

Retirar un node id desconocido también registra el retiro, porque un retiro que no
hace nada en silencio porque el id estaba mal escrito es el modo de fallo que
importa: el operador se va creyendo que el dispositivo está fuera de la red.

La rotación es una llamada de aprovisionamiento, y `POST /v1/provision` responde
`"reinstated": true` cuando reinscribe un nodo retirado, para que una rotación no
tenga que descubrirse después en los logs.

## Aprovisionamiento

`backend/tools/provision.py` genera una semilla Ed25519 por dispositivo al final de
línea, instala la clave **pública** en el central, y escribe un manifiesto `0600`:

```
python tools/provision.py --central https://cauce.example --count 24 \
    --site rio-01 --out provisioning.json
python tools/provision.py --central https://cauce.example --manifest provisioning.json
python tools/provision.py --central https://cauce.example --retire CAUCE-014
```

Al central nunca se le da una semilla. No hay a propósito ninguna ruta de
"recupera mi semilla": añadirla convertiría el manifiesto de un registro de
aprovisionamiento en una custodia de claves, que es otro producto con otro modelo de
amenaza. Una semilla perdida es una identidad perdida: reaprovisionar y retirar la
clave vieja.

En Windows `chmod` no restringe nada, así que la herramienta advierte que la
restricción no se aplicó en lugar de dar a entender que sí.

## Algoritmos de firma

Los frames se autentican por nodo, y el algoritmo lo fija el aprovisionamiento en
lugar de elegirlo el request — un frame no puede reetiquetarse para que el central
ejecute la comprobación que prefiera. Lo que despacha el central es la longitud del
trailer.

| | trailer | lo que aprende quien lee la base del central |
|---|---|---|
| HMAC-SHA-256 | 32 B | el secreto compartido, así que cualquier nodo HMAC es suplantable |
| Ed25519 | 64 B | una clave pública, así que no puede firmar |

El firmware guarda la **semilla** Ed25519, nunca el escalar expandido, para que quien
llame no pueda aprovisionar ni persistir la mitad equivocada. Ed25519 exige
exactamente una semilla de 32 bytes: 1, 16, 31, 33 y 64 bytes se rechazan todos,
porque una semilla rellenada o truncada firma perfectamente y no verifica en ningún
sitio, y el único síntoma es un central que descarta en silencio todos los frames.

El Ed25519 de este firmware **no es de tiempo constante**: los acarreos y la escalera
escalar operan sobre valores derivados de la clave. Un relé o un central hostil solo
ve la firma y no le afecta; un atacante capaz de medir el tiempo de firma en local
con alta resolución sí podría. Donde el propio dispositivo deba considerarse hostil,
usar una biblioteca de tiempo constante.

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
