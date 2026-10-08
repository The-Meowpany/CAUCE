# Procedimiento de calibración

Cómo llevar las lecturas de un nodo a una escala defendible, y cómo saber cuándo no hacerlo.

La herramienta es `backend/tools/calibrate.py`. Este documento es la parte que no se puede
automatizar: cómo es una buena co-localización, qué resultados se aceptan y cuáles se tiran.

## Por qué existe la calibración

Un BME280 no es un termómetro. Su precisión absoluta es de aproximadamente ±1 °C de fábrica,
que es del mismo orden que la variación entre días que el sistema existe para observar. Un
emplazamiento con una oscilación diurna de 0,3 °C no puede tener esa oscilación medida por un
sensor sin calibrar, ni por uno mal calibrado: una corrección lineal no elimina la histéresis,
y un BME280 se calienta y deriva.

Así que la calibración no es un detalle aquí; sin ella el central no puede distinguir un cambio
real en el entorno de un cambio en el sensor.

## El procedimiento

### 1. Co-localizar

Un nodo y un instrumento de referencia, **en el mismo aire**, durante al menos 48 horas.

«Mismo aire» es toda la dificultad, y es física:

- **Blindaje radiativo.** Un nodo al sol directo lee varios grados por encima de la temperatura
  del aire. Es la causa más común de una calibración mala y no se ve mala en la gráfica: la
  correlación se mantiene alta mientras la desviación sigue al clima. Ponga ambos bajo un
  blindaje blanco ventilado, o como mínimo fuera del sol directo y lejos de una pared caliente.
- **Altura.** Dentro de unos 30 cm, o está midiendo un gradiente.
- **Suelo.** No sobre el suelo. Un nodo sobre suelo desnudo ve la temperatura del suelo, y el
  suelo tiene una oscilación diurna mucho mayor que el aire.
- **Carcasa.** Si el nodo está en una caja estanca, la caja es el instrumento. Calibre lo que
  realmente se despliega.

### 2. Recoger

```sh
python tools/calibrate.py --site SITE-01 --variable air_temperature \
    --reference NODE-REF --hours 72 --json > calibration-raw.json
```

72 horas en lugar de 48 si hay margen. Dos ciclos diurnos completos es el mínimo; un solo ciclo
no separa una corrección lineal de un desplazamiento de fase, y la ferramenta ajustará una
corrección lineal que está equivocada con toda confianza.

La referencia debe estar calibrada a su vez. Una calibración contra una referencia sin calibrar
es un efecto de segundo orden que este procedimiento no detecta.

### 3. Leer el informe antes de aceptarlo

El informe lleva una fracción de coincidencias, una incertidumbre y el propio ajuste. **La
fracción de coincidencias es la puerta.** `calibrate.py` la informa como número, y el
procedimiento es: por debajo de 0,90, tire la ejecución e investigue por qué.

| fracción de coincidencias | lectura | acción |
|---|---|---|
| ≥ 0,98 | excelente | aceptar |
| 0,90 – 0,98 | aceptable | aceptar, dejarlo anotado |
| 0,70 – 0,90 | mala | rechazar; la co-localización no estaba co-localizada |
| < 0,70 | fallida | rechazar e investigar el hardware |

Una fracción de coincidencias baja no es ruido. Es el informe diciendo que los dos instrumentos
no vieron lo mismo, y las causas habituales están en este orden:

1. Uno de ellos estuvo al sol directo durante parte de la ventana.
2. No estaban a la misma altura, o uno estaba en el suelo.
3. Uno perdió Wi-Fi durante parte de la ventana, así que sus marcas de tiempo son incorrectas y
   se está comparando contra la hora equivocada. Compruebe el número de muestras; una ejecución
   que descarta medio día en silencio es la versión de esto que cuesta una semana.
4. La propia referencia derivó.

### 4. Ajustar

Por defecto solo desplazamiento, es decir `medido = crudo + desplazamiento`.

`--fit-scale` añade una pendiente, por mínimos cuadrados. Úselo cuando los residuales muestren
curvatura —el error es proporcional a la lectura en lugar de constante—, lo cual es habitual en
la humedad del BME280.

**Una pendiente cercana a 1,0 con un desplazamiento grande es el resultado normal.** Una
pendiente lejos de 1,0 significa que un instrumento no es lineal en este rango, lo cual en un
BME280 suele significar que está fuera de su banda de operación especificada. Compruebe el rango
antes de aceptar una pendiente.

### 5. Aplicar

```sh
python tools/calibrate.py --site SITE-01 --variable air_temperature \
    --reference NODE-REF --hours 72 --apply --central https://central.example --token "$ADMIN_TOKEN"
```

`--apply` se niega sin un token de administrador, y la API también lo rechaza sin él.

### 6. Verificar, que no es opcional

Aplique a **un nodo**, espere un ciclo diurno completo, y después compare ese nodo contra la
referencia otra vez *sin* aplicar nada.

- El residual baja: acepte, y luego aplique al resto del emplazamiento.
- El residual no baja: el ajuste estaba mal, o la corrección se aplicó a otra variable, o el
  nodo derivó después de calibrarse. Deshágala y empiece de nuevo.

Aplicar a todo el emplazamiento y averiguarlo después es como un mal desplazamiento se convierte
en una semana de datos que se lee como una señal climática.

## Recalibración

Cada **90 días**, e inmediatamente después de:

- una actualización de firmware que cambie el driver del sensor,
- la sustitución de un sensor,
- el traslado del nodo a otra posición,
- un cambio en la carcasa o en el blindaje.

La sustitución de un sensor invalida la calibración por definición. Un nodo que se ha movido
está midiendo otra cosa, y el desplazamiento antiguo es peor que no tener desplazamiento,
porque está equivocado con toda confianza.

## Lo que la calibración no puede arreglar

Se dice aquí porque la tentación es seguir iterando sobre el ajuste:

- **Histéresis.** El BME280 lee de forma distinta al subir que al bajar. Una corrección lineal
  es simétrica por construcción y no puede representar esto.
- **Condensación.** Un nodo que se moja lee mal de una manera que ningún desplazamiento corrige.
- **Auto-calentamiento.** Un nodo en una carcasa estanca con una radio Wi-Fi calienta su propio
  aire. Es un sesgo fijo que un desplazamiento *sí* puede eliminar, pero la solución es la
  carcasa, y eliminar el sesgo esconde un problema que va a empeorar.
- **Un sensor fallido.** Un BME280 atascado produce una línea plana, y una línea plana puede
  producir un ajuste de aspecto excelente contra otra línea plana. Compruebe que la referencia se
  movió realmente durante la ventana antes de aceptar nada.

## Procedencia

Cada calibración aplicada registra su nodo de referencia, su ventana, sus parámetros de ajuste y
la incertidumbre. Eso es lo que hace respondible una pregunta posterior —«¿se corrigió este
valor, y cuándo?»— y es la razón por la que el central se niega a aceptar una calibración sin
referencia.