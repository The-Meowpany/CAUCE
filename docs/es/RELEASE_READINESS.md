# Preparación de release: qué falta para congelar el producto

`STATUS.md` dice qué existe. `ROADMAP.md` dice hacia dónde crecerá el producto.
Este archivo responde una pregunta más estrecha: **qué falta para poder declarar
cerrado el conjunto de funcionalidades y entregar el resultado como algo fijo.**

## El vocabulario, porque se preguntó

La secuencia tiene nombres estándar, y no son lo mismo:

| Término | Significado |
|---|---|
| **Feature freeze** | Fecha tras la cual no entra ninguna funcionalidad nueva. Solo defectos. |
| **Code freeze / RC** | Release candidate: se corta el árbol, solo correcciones. |
| **GA (general availability)** | La release tal como se vende. |
| **Baseline** | El conjunto de artefactos congelados: imagen de firmware, tabla de particiones, versión de backend, versión de esquema, documentación. Esto es lo que se quiere decir con "la versión que desplegamos". |
| **LTS / solo mantenimiento** | Funcionalidades congeladas, pero los defectos de seguridad y críticos se parchean durante un periodo declarado. |
| **EOL** | Ningún parche más, de ningún tipo. |

El encuadre honesto: **se puede congelar el conjunto de funcionalidades; no se
puede congelar la ausencia de parches.** Este producto termina TLS, verifica
firmas Ed25519 y escribe su propia flash. Una build sin canal de parches es un
pasivo, no un producto terminado. El estado objetivo es una **baseline congelada
bajo mantenimiento (LTS)**, con EOL definido como fecha y no anunciado en la
entrega.

---

## Fase 0 — Congelar la especificación

No se construye nada hasta que los no-objetivos estén escritos, porque "completo"
no está definido sin ellos.

- [ ] Una página que diga qué **no** hace el producto, por subsistema.
- [ ] Decidir las dos preguntas que cambian la factura de hardware:
  - **¿Entra ESP-NOW/mDNS en la baseline?** El merge y las interfaces existen; la
    radio no. O se financia o se difiere explícitamente.
  - **¿Se entrega el camino LoRa?** Está probado en host de punta a punta pero no
    tiene driver de radio ni link budget, así que la interfaz de aire no está
    probada.
- [ ] Congelar los formatos en el cable: frame LoRa, envelope de sync, recursos
    REST, frame de almacenamiento, tabla de particiones. Desde aquí se versionan, no
    se editan.
- [ ] Congelar el esquema de base de datos y comprometerse a migraciones
    disciplinadas desde hoy.

**Puerta:** alguien que nunca haya visto el proyecto puede decir, en una página,
qué es el producto y qué no.

---

## Fase 1 — Cerrar los huecos de software (solo escritorio)

No necesitan hardware. Todos son reales y todos son visibles en `STATUS.md`.

### 1.1 Ed25519 en el transporte — **obligatorio**

`ed25519Sign` / `ed25519Verify` existen y pasan los vectores de RFC 8032, pero el
transporte de firmware firma **solo con HMAC-SHA-256**. El central ya soporta ambos
algoritmos por nodo (`device_key_algorithm`). Hasta que el firmware pueda elegir,
todo nodo es simétrico y la historia asimétrica de la tesis no es cierta de los
dispositivos entregados.

- [ ] `LoRaSyncTransport` toma algoritmo y semilla, no una clave compartida.
- [ ] El trailer del frame es de 32 bytes para HMAC y 64 para Ed25519, y el
      despacho por longitud que ya tiene el central se ejercita con un test para
      cada uno.
- [ ] Firma medida en el objetivo: en un ESP32 una firma son milisegundos, no
      microsegundos. Fijar el número y ponerlo en `docs/en/SECURITY.md`.

### 1.2 Actuación de downlink

Las clases de downlink validan y reportan pero no reconfiguran un nodo.

- [ ] `relay` / `sample_interval` / `calibrate` se aplican y se persisten.
- [ ] El acuse reporta el valor aplicado, no el pedido.

### 1.3 Calibración como método, no como capa

Los registros llevan incertidumbre absoluta y la escalan con la corrección. No hay
procedimiento formal ni trazabilidad, así que un número no se puede defender.

- [ ] Procedimiento escrito por magnitud: referencia, método, número de puntos,
      criterio de aceptación, intervalo.
- [ ] Presupuesto de incertidumbre por magnitud, propagado al informe: ya está
      conectado, falta llenarlo.
- [ ] Certificados o registros de referencia retenidos, para que una calibración
      sea auditable.

### 1.4 Integridad de la cobertura

Una consulta está acotada a 400 días y reporta los 20 huecos principales con
`gaps_truncated`. Acotado, no completo.

- [ ] Decidir si el tope es aceptable para un piloto y escribir esa decisión, o
      paginar el endpoint de huecos.
- [ ] Mostrar `gaps_truncated` en el dashboard, no solo en la API.

### 1.5 Aislamiento del binario de tests

Un solo binario Unity para todas las suites, aislado por directorios de datos.
Funciona hoy, pero una suite quefiltration estado acabará pasando por el motivo
equivocado.

- [ ] Separar binarios por suite, o añadir un teardown que resettee el estado de
      forma demostrable.

### 1.6 Operación del central

- [ ] Un certificado real y un nombre DNS. `deployment/` trae Caddy con CA
      interna: bien en LAN de confianza, mal en internet.
- [ ] Backup y restore, ejercitados al menos una vez de verdad. Un backup sin probar
      es una hipótesis.
- [ ] Migraciones de esquema reversibles y numeradas desde este punto.
- [ ] Confirmar que los topes de retención coinciden con el volumen esperado del
      piloto (`docs/en/PILOT_SPEC.md`).

**Puerta:** `scripts/verify-all.ps1` en verde, más una nota de release firmada y
`SECURITY.md` actualizado.

---

## Fase 2 — Cerrar la interfaz de aire

### 2.1 Radio LoRa y link budget — **obligatorio si LoRa se entrega**

- [ ] Driver SX1276 detrás de la interfaz `ILoRaRadio` ya probada.
- [ ] Link budget escrito: factor de署ctamiento, ancho de banda, payload, airtime
      por frame, duty cycle, y el peor caso de nodos por gateway.
- [ ] Medido, no calculado: RSSI, SNR, tasa de error de paquete a distancia.
- [ ] Regulatorio: límites de duty cycle y certificación de banda para la región de
      despliegue (`docs/en/LEGAL.md`).

### 2.2 ESP-NOW / mDNS punto a punto — **decidir antes de cerrar la Fase 0**

`Replication.h` e `IPeerLink.h` existen y están probados; el transporte no.

- [ ] Entregarlo: `IPeerDiscovery` sobre mDNS, `IPeerRadio` sobre ESP-NOW, y el
      bucle de intercambio cableado en `main.cpp`.
- [ ] O diferirlo. Diferir es defendible — la propia tesis argumenta que no hace
      falta mesh — pero entonces se difiere *explícitamente*, no se deja como una
      interfaz sin implementación.

### 2.3 Deep sleep activo por defecto

Cableado pero desactivado a la espera de la medición de banco. No se puede activar
en las Fases 0-2.

---

## Fase 3 — Validación en banco (solo hardware)

`docs/en/BENCH_PLAN.md` es el procedimiento. Lo que falta es el **resultado**.
Cada punto debe registrar números medidos, no "funciona".

| # | Punto | Criterio de paso |
|---|---|---|
| B1 | BME280 sobre Wire real | Las lecturas coinciden con un instrumento de referencia dentro de la incertidumbre declarada |
| B2 | LittleFS en flash real | Montar, llenar, cortar alimentación a mitad de escritura, recuperar; cifra de desgaste registrada |
| B3 | Radio Wi-Fi | Reconecta tras pérdida de AP y tras ciclo de energía; tiempo medido hasta el primer sync |
| B4 | OTA en flash real | Escribe, arranca, confirma, y hace rollback con una imagen deliberadamente mala |
| B5 | Fuente de tiempo NTP | Desvío medido contra un reloj de referencia; comportamiento sin conectividad registrado |
| B6 | Radio LoRa | Link budget como en 2.1, a distancia |
| B7 | Consumo | Corriente en deep sleep medida; el intervalo resultante es lo que activa 2.3 |
| B8 | Carcasa y térmica | El sensor lee su ambiente, no su propio calor |

**Puerta:** B1-B8 registrados con números en `BENCH_PLAN.md`. Hasta entonces el
estado honesto es "probado en host, no demostrado en hardware", que es lo que dice
`STATUS.md` hoy.

---

## Fase 4 — Fabricación y aprovisionamiento

Sin empezar, y es la fase que más se olvida hasta que urge.

- [ ] **Inyección de semilla por dispositivo al final de línea.** Cada nodo recibe
      su propia semilla Ed25519, generada en línea, nunca compartida entre unidades.
- [ ] **Cómo se protege la semilla.** Está en el dispositivo y no es extraíble por
      software. Decir sin rodeos en `SECURITY.md` qué gana un atacante físico.
- [ ] **Qué pasa si se pierde una semilla.** Una semilla perdida es una identidad
      perdida: el nodo se reaprovisiona y la identidad vieja se revoca en el
      central.
- [ ] **Test de fábrica.** Un comando que demuestre que una unidad funciona antes de
      salir: lectura de sensor, escritura en almacenamiento,join Wi-Fi, sync
      firmado, downlink firmado.
- [x] **Manifiesto de aprovisionamiento.** id de dispositivo, semilla, clave de manifiesto,
      algoritmo y sitio registrados una vez, en un archivo que se pueda releer en soporte.
- [x] **Revocación y rotación.** `POST /v1/nodes/{id}/revoke` retira una
      identidad y toda ruta autenticada llama antes a `require_not_retired`. La jubilación
      no es borrado: las mediciones que aportó el nodo se quedan, porque un
      dispositivo comprometido no convierte retroactivamente en falsas sus datos;
      lo que se retira es el derecho a aportar más.

## Fase 5 — Ingeniería de release

- [ ] Versionado semántico aplicado, con el esquema y los formatos en el cable como
      fronteras explícitas de versión mayor.
- [ ] Una etiqueta de release, y el conjunto de artefactos fijado: `.bin` de
      firmware, `partitions.csv`, imagen de backend, fixtures, documentación.
- [ ] SBOM de las dependencias del backend (`cryptography` es la que importa).
- [ ] Build de backend reproducible, o al menos un hash de lockfile registrado.
- [ ] **Un script de puerta de release** que se niegue a etiquetar si: `verify-all`
      en verde, tests de firmware verdes en la configuración objetivo, backend verde,
      E2E verde, índice de documentación coherente con el árbol.
- [ ] Un runbook: qué hacer cuando un nodo enmudece, cuando el central cae, cuando
      crece una cola de sync, cuando se envía una imagen mala.

---

## Fase 6 — Congelar, y luego solo mantenimiento

La declaración, y debería ser un solo documento:

1. **El feature freeze está vigente.** Nueva funcionalidad entra en `v2`, no en `v1`.
2. **La baseline está identificada**: versión, etiqueta git, hashes de artefactos,
   versión de esquema, versiones de formato.
3. **La ventana LTS está declarada**: cuánto tiempo se parchean seguridad y defectos
   críticos, y quién lo hace.
4. **La fecha EOL está declarada.** Después, no hay parches, y el central deja de
      aceptar esos nodos. Decirlo de antemano en lugar de descubrirlo.
5. **La lista congelada de puntos diferidos se publica**, para que nadie asuma que
      se olvidaron.

---

## Diferido explícitamente a v2

Diferir es una decisión, no un olvido. Cada uno de estos es razonable *no*
tenerlo en una baseline congelada, siempre que quede escrito:

- Mesh punto a punto (si 2.2 lo difiere) — la tesis argumenta que no hace falta
- Visualización volumétrica 3D — ya descartado por decisión de diseño
- Actuación física de actuadores por downlink LoRa — requiere hardware actuable
- Trazabilidad metrológica formal más allá de 1.3
- Central multi-tenant, si el objetivo es un solo operador

---

## La puerta, en un solo sitio

Una baseline congelada requiere todo esto:

- [ ] Fase 0 aprobada: no-objetivos escritos
- [ ] Fase 1 completa: el firmware firma con Ed25519, el downlink actúa, la
      calibración tiene procedimiento, el central tiene certificado real y un
      restore probado
- [ ] Fase 2 decidida: LoRa entregado con link budget medido, o diferido por
      escrito; ESP-NOW entregado o diferido por escrito; deep sleep activo con
      corriente medida
- [ ] Fase 3 completa: B1-B8 registrados con números
- [ ] Fase 4 completa: semillas por dispositivo, test de fábrica, revocación
- [ ] Fase 5 completa: baseline etiquetada, SBOM, script de puerta, runbook
- [ ] `scripts/verify-all.ps1` en verde sobre el árbol etiquetado exacto
- [ ] Ventana LTS y fecha EOL publicadas

Lo que quede sin marcar significa que la respuesta honesta es "un prototipo muy
bueno", no un producto terminado. Es una buena cosa. No es lo mismo.
