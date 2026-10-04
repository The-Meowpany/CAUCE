# Runbook: qué hacer cuando algo falla en campo

Escrito para quien esté de guardia a las 03:00 y nunca haya usado este sistema.
Cada entrada es un síntoma reconocible desde un log o un dashboard, la
comprobación que lo confirma, y la acción.

La inclinación de todo el documento es **no perder datos**. Un nodo que se calla es
un problema; un nodo borrado es peor. Nada de esto destruye mediciones.

---

## Un nodo dejó de reportar

**Reconocerlo:** el dashboard muestra un sitio apagado; `GET /v1/nodes` muestra
`last_seen_utc_ms` con horas de antigüedad.

**Comprobar, en este orden.** Lo más barato y más probable primero.

1. ¿Está vivo el central? `curl -k https://<host>/healthz`. Si esto falla, todos los
   nodos parecen muertos y ninguno lo está.
2. ¿El sitio responde de alguna manera? ¿Reporta algún nodo de ese sitio?
3. ¿Es un nodo o son muchos? Muchos de un sitio apunta al gateway o a la radio. Uno
   apunta al dispositivo.
4. Preguntarle al dispositivo. Su API local responde en la LAN aunque no tenga
   subida: `GET /api/v1/health` da `netState`, `rssiDbm`, `storedCount`,
   `measurementCount`. Un nodo con `storedCount` creciendo y `netState` muerto tiene
   datos que no puede enviar — eso es un problema de radio, no de datos, y los datos
   están a salvo en el dispositivo.

**Actuar:** nada destructivo. Un nodo que estuvo offline acumula en flash y sincroniza
al reconectarse; `(node_id, sequence)` hace que la recuperación sea idempotente, así
que un nodo que vuelve tras una semana no necesita que se le haga nada. Anotar en el
log que estuvo callado, porque un nodo que pasa su presupuesto de almacenamiento
empieza a descartar.

---

## La cola de sincronización crece

**Reconocerlo:** `pending` en el nodo sube, o `sync_batches` del central crece más
rápido de lo que se acusa recibo.

**Comprobar:** ¿el nodo transmite y no recibe acuse? `LoRaSyncTransport` expone
`lastDeliveryConfirmed()`. Si los frames salen y nunca se confirman, el gateway no
los oye o no los reenvía; `/v1/nodes/{id}` del central mostrará `last_seen`
avanzando aunque las mediciones no lleguen.

**Actuar:** subir el factor de署ctamiento si la radio lo permite. SF9 cabe un
registro por frame; cada paso arriba cuesta airtime y compra 3 dB. No subir el
intervalo de sincronización para "ponerse al día": eso empeora la cola.

---

## El central está caído

**Actuar:** los nodos no se ven afectados. Miden, validan, almacenan, sirven su
propio dashboard. Esa es la premisa del diseño, no una consolesa.

**Antes de arrancarlo**, hacer backup si el sistema de archivos lo permite:

```
curl -k -H "Authorization: Bearer $TOKEN" \
  https://<host>/v1/maintenance/backup -o backup-$(date +%F).sqlite
```

**Después de arrancarlo**, confirmar que las migraciones corrieron:

```
curl -k https://<host>/healthz
```

Si lo que rompió fue una migración, restaurar la base anterior con
`backend/tools/restore.py` **con el servicio detenido**.

---

## Restaurar el central desde un backup

```
# ver qué hay en el archivo, sin cambiar nada
python backend/tools/restore.py --backup backup.sqlite --db ./data/cauce.sqlite --dry-run

# detener el servicio, luego
python backend/tools/restore.py --backup backup.sqlite --db ./data/cauce.sqlite

# arrancar el servicio, luego
curl -k https://<host>/v1/nodes
```

La herramienta rechaza un archivo corrupto, uno que no sea una base de CAUCE, y uno
con menos filas que la base viva salvo que se pase `--yes`. Ese último rechazo es el
que aparece al restaurar por error el archivo de ayer; leer los conteos que imprime
antes de pasarlo por alto.

---

## Un dispositivo se perdió, fue robado o está comprometido

**Actuar — retirarlo.** Es lo único que hay que hacer rápido.

```
curl -k -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"reason":"stolen"}' \
  https://<host>/v1/nodes/CAUCE-014/revoke
```

Sus mediciones se quedan. Lo que se retira es su derecho a aportar más.

**Confirmar que funcionó:** un sync de ese nodo devuelve ahora `403 node_retired`,
tanto por Wi-Fi como por la ruta reenviada.

**Si el dispositivo se recupera y es confiable**, para el central es un dispositivo
nuevo. Generar una semilla nueva y volver a aprovisionar:

```
python backend/tools/provision.py --central https://<host> --count 1 \
  --prefix CAUCE --site rio-01 --out reseed.json
```

La semilla vieja está muerta. No reutilizarla.

---

## Un nodo no acepta un comando

**Reconocerlo:** el central lo sigue ofreciendo y el nodo nunca acusa recibo.

**Comprobar:** `request_resync`, `set_sampling_interval`, `set_sync_interval` y
`set_led_mode` son las clases implementadas. Cualquier otra se reporta como clase
desconocida y nunca será acusada — eso es un error del central, no del nodo.

Para las cuatro reales, el motivo del rechazo está en el detalle del recibo:

| detalle | significado |
|---|---|
| `refused:sampling_out_of_range` | menos de 10 s o más de 86400 s |
| `refused:sync_out_of_range` | menos de 60 s o más de 86400 s |
| `refused:silence_budget_s=604800` | el par dejaría al nodo callado más de una semana |
| `refused:led_mode_out_of_range` | modo mayor que 3 |
| `applied:...` | funcionó; el valor tras `applied:` es el que el nodo hace ahora |

**Un recibo con `applied:` es el valor vigente, no el pedido.** Si un comando fue
limitado o rechazado, el nodo lo dice.

---

## Una actualización dejó un nodo inservible

**Reconocerlo:** el nodo no responde y tampoco contesta su API local.

**Comprobar:** casi todos los "ladrillos" son nodos que arrancaron una imagen que
nunca confirmó. El guard de rollback debería devolverlo a la partición anterior en el
siguiente arranque; si no vuelve en dos ciclos de arranque, necesita acceso físico.

**Actuar:** reflashear por USB. `docs/es/OTA.md` tiene el procedimiento.

**Prevención:** las reglas anti-ladrillo de `docs/es/OTA.md` existen para que esto no
pueda pasar por una descarga mala — hash, tamaño y compuertas de seguridad se
comprueban antes de escribir un byte. Todavía puede pasar por una imagen válida y
equivocada, que es para lo que está la confirmación por contador de arranques. **Ese
mecanismo está probado en host y no demostrado en hardware**, así que hasta que el
banco lo confirme, tratar cada OTA como si alguien tuviera que estar cerca.

---

## Alguien pregunta si un nodo está comprometido

La respuesta es acotada y los límites se conocen:

- **Nodos HMAC.** El nodo y el central comparten un secreto. Quien haya leído la base
  del central puede suplantar cualquier nodo HMAC. Quien haya leído el flash de un
  nodo puede suplantar ese nodo.
- **Nodos Ed25519.** El central solo guarda una clave pública, así que leer su base
  **no** permite firmar. Leer el flash de un nodo **sí**, porque la semilla está en el
  dispositivo.
- **Ambos.** No existe firma de firmware. Un nodo aceptará y ejecutará cualquier
  imagen que satisfaga el manifiesto, así que quien pueda servir el manifiesto puede
  ejecutar su propio código en el nodo.

Esas tres frases son la postura de seguridad real. Cualquier cosa más precisa que
ellas no está implementada.

---

## Reportar un fallo

Incluir, porque todo esto ha hecho falta para diagnosticar algo:

- el node id y el sitio
- `last_seen_utc_ms` de `GET /v1/nodes/{id}`
- `GET /api/v1/health` del dispositivo si responde
- la línea de log del central, textual
- si el fallo es un nodo fijo, un sitio entero, o todo

Nodo fijo apunta al dispositivo. Sitio entero apunta al gateway o a la radio. Todo
apunta al central o al enlace. Esa clasificación son tres preguntas y resuelve la
mayor parte.