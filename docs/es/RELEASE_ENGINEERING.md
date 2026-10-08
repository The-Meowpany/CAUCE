# Ingeniería de release

Qué tiene que ser cierto antes de que una etiqueta signifique algo, y qué está demostrando en
realidad cada parte de `scripts/release-gate.ps1`.

## El principio

Una puerta de release que informa en verde tiene que estar informando de algo verdadero. Esa es
una exigencia más fuerte que «los tests pasan», y la diferencia es donde se fue casi todo el
trabajo de este proyecto.

Dos modos de fallo costaron tiempo real aquí, y ambos tienen la misma forma:

- **Una afirmación que nada comprueba.** Los totales de tests documentados eran prosa. La suite
  pasó de 325 → 345 → 349 a lo largo de tres commits y cuatro documentos siguieron citando la
  cifra antigua, porque nada ejecutable las comparaba. `release-gate.ps1` ahora lee los recuentos
  de la salida de `verify-all.ps1` y rechaza un release cuyos documentos no coincidan.
- **Una comprobación que no puede fallar.** La primera versión de esa comprobación exigía una
  línea del README que dijera `"541 firmware"`, porque reutilizó el patrón de firmware para el
  recuento del backend. Falló en voz alta, que es la única razón por la que se pudo encontrar.
  Todas las comprobaciones de aquí abajo se verificaron provocándolas a fallar, no leyéndolas y
  estando de acuerdo en que funcionarían.

## Qué demuestra la puerta, y qué no

`scripts/release-gate.ps1 -Tag v0.1.0`

| Comprobación | Qué establece realmente |
|---|---|
| el árbol está limpio | no hay release sin commitear |
| versión y etiqueta | la etiqueta nombra a este commit |
| la imagen del backend está fijada | el digest de la base es el que esta puerta espera, no el que está en el fichero que está comprobando |
| verificación completa | tests de firmware, build ESP32, suite del backend, E2E vivo — y que los totales documentados coinciden con lo que se ejecutó |
| artefactos de flash ESP32 | esptool parsea el bootloader y la aplicación; dos slots de aplicación, sin solape, alineados a sector; la imagen cabe en su slot |
| imagen de contenedor | construye desde el Dockerfile real, sirve `/healthz` por HTTP, rechaza escrituras sin token de administrador, pasa la suite dentro de la imagen, y dos construcciones de un árbol producen un mismo ID de imagen |
| cierre fijado | cada hash de dependencia verifica y la aplicación se ejecuta desde él en un virtualenv que no contiene nada más |
| SBOM | cada pin del SBOM coincide con el lock |
| la CA falla cerrada | sin clave de CA configurada, los endpoints de certificado son `503` |
| documentación | cada documento que el índice lista existe, en ambos idiomas |
| paridad en/es | cada documento de `docs/en` tiene su contraparte en `docs/es`, salvo los que estén en la lista explícita de solo-ingles |
| el firmware firma | el códec declara, firma y selecciona ambos algoritmos, y rechaza una longitud de clave incorrecta |
| orden del grupo | la constante del orden del grupo Ed25519 sigue siendo la verificada |
| ningún test sin registrar | ningún fichero de test de firmware menciona `NOT REGISTERED` |

## Lo que no demuestra

Dicho sin rodeos, porque el valor de una puerta está enteramente en el límite de su afirmación:

- **Nada de hardware.** Nada de esto ejecuta firmware en un ESP32. QEMU modela dc232b y lx6 y no
  tiene máquina esp32, así que no hay emulador que lo permita. Las comprobaciones de flash son lo
  máximo que se puede saber sin hardware, y una placa todavía puede suspender todas ellas
  reiniciándose.
- **La imagen de firmware no es reproducible entre directorios.** Incorpora `app_elf_sha256` y
  un digest de imagen añadido, ambos calculados sobre un fichero cuyas secciones de depuración
  llevan rutas absolutas. Dos clones de un mismo commit en rutas distintas difieren en 65 de
  1.115.984 bytes. Por eso `artifacts.sha256` se compara, nunca se reescribe: reescribirlo hacía
  que la primera ejecución de la puerta dejara el árbol sucio y que la segunda fallara por un
  motivo ajeno al código.
- **La imagen arm64 está en el índice fijado pero nunca se ha ejecutado.** Esta máquina es amd64.
- **El intercambio de certificados nunca se ha encontrado con una placa.** Ambas mitades compilan
  y ambas se prueban contra los bytes de la otra; nada ha demostrado que interoperen en silicio.

## Versionado

`0.1.0` es deliberado y el razonamiento está en el mensaje de la etiqueta: la especificación no
está congelada, las fases 0 y 2 no han empezado, y la placa nunca ha estado sobre una mesa. Un
`1.0.0` aquí sería una afirmación sobre preparación que nada en el repositorio respalda.

La etiqueta es anotada y su mensaje son las notas del release. Ocupa unos 18 kB y enuncia lo que
el release **no** establece, porque una nota de release que solo lista logros es un documento de
marketing.

## Cadencia de release

Ninguna, deliberadamente, hasta que haya una flota a la que publicar. Cuando la haya:

- **Patch** — una corrección, sin cambio de esquema, sin cambio de protocolo.
- **Minor** — una adición retrocompatible: un endpoint nuevo, un campo nuevo, una versión de
  firmware que lee y escribe el mismo formato.
- **Major** — cualquier cosa que haga que un central antiguo rechace un nodo nuevo o al revés.
  Esto incluye un cambio en la codificación canónica del challenge, en el formato de frame
  firmado, o en la paginación de cobertura, porque cada uno de ellos tiene un nodo al otro lado.

## Procedimientos de rotación

Dos credenciales necesitan rotación con periodicidad, y ambas están documentadas donde se usan:

- **`CAUCE_CA_KEY`** — la CA de identidad de nodo Ed25519. `docs/es/SECURITY.md`. La rotación
  reemplaza en lugar de acumular, conservando la fila retirada, porque una tabla que destruye el
  certificado antiguo no puede responder qué clave tenía un nodo cuando llegó una medición.
- **La CA X.509 de `backend/tools/pki.py`** — para el TLS del propio central. `pki.py inventory`
  lista cada certificado y su validez restante; avisa dentro de los 90 días para una CA. Es una
  segunda raíz de confianza a propósito: la CA Ed25519 firma JSON y no puede presentarse en un
  handshake TLS, así que no es una alternativa de esta, es algo distinto.

## Cómo ejecutarla

```sh
./scripts/verify-all.ps1                     # rápido: tests, build ESP32, backend, E2E vivo
./scripts/release-gate.ps1 -Tag v0.1.0       # la puerta completa, incluidos contenedores
./scripts/release-gate.ps1 -SkipContainer    # sin podman; las comprobaciones de contenedor se omiten y se anotan
```

La puerta es idempotente por construcción y eso está probado: se ha ejecutado dos veces seguidas
sobre el mismo clon, pasando ambas veces, con el árbol limpio después de cada una. Una puerta
cuya segunda ejecución falla no es una puerta.