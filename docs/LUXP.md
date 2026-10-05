# .luxp — el formato de aplicación de Luxium

Un `.luxp` es un formato propio: cada página se guarda ya convertida en las estructuras con las que trabaja el motor. **Lux lo produce, Luxium lo abre.** HTML + CSS + LuxScript es la fuente a partir de la cual Lux sabe generarlo hoy.

```
lux pack site/index.html app.luxp      # un sitio estático (todas las páginas enlazadas)
render("x.html", ...)                  # una ruta de Lux: .luxp para Luxium, HTML para el resto
luxium app.luxp                        # abrirlo
luxium http://servidor-lux/ruta        # o pedirlo por red
luxium --inspect app.luxp              # verificarlo y ver todo lo que puede hacer
```

## Quién hace qué

| pieza | dónde vive | qué hace |
|---|---|---|
| Formato | `Lux/src/luxp/`, vendorizado en Luxium (`tools/sync_from_lux.sh`) | Estructuras DOM, parsers HTML/CSS, códecs de cada sección, verificador de bytecode, compilación de scripts de página y el **traductor** `translate_html`. |
| API del navegador | `Lux/include/lux_script/browser_api.inc`, vendorizado | La lista de nativas, objetos reservados y métodos que ve una página. Lux compila contra ella (`BrowserProfile`); Luxium construye sus tablas reales con ella. |
| Producción | Lux: `lux pack`, `render()`, `lux --check` | Traduce, comprueba y sirve. |
| Consumo | Luxium | Monta, verifica, ejecuta. |

**Un único traductor.** Luxium también muestra los HTML locales pasándolos por `translate_html`. Una página HTML y su `.luxp` no pueden interpretarse de dos formas distintas: [tools/luxpequiv.py](../tools/luxpequiv.py) lo comprueba y da 123 de 123 idénticas píxel a píxel con `.luxp` generados por `lux pack`.

`tools/sync_from_lux.sh --check` falla si la copia de Luxium se separa de Lux. `natives.hpp`, `natives.cpp` y `template.cpp` de `lux_script` son propios de Luxium a propósito y no se sincronizan.

## Plantillas de Lux

Las plantillas siguen siendo las de siempre: `{{ }}` y `{% %}` valen en cualquier sitio, incluido el `<script>` de la página (`string quien = "{{ who }}"`). Lo que cambia según la plantilla es **cuánto cuesta** generar su `.luxp`, de más barato a más caro:

**1. Plan: la plantilla compilada a `.luxp` (el camino rápido).** Si ningún `{{ }}` ni `{% %}` está donde la estructura de la página dependa de los datos, la plantilla se traduce **una sola vez, al compilar**, a un documento con huecos, más su hoja de estilos y su programa (los dos son iguales sea cual sea el dato, y se comprimen una sola vez). Por petición solo se evalúan los huecos, con el mismo motor de plantillas, y se escriben los bytes: ni tokenizer HTML, ni parser CSS, ni compilador, ni compresión.

Cómo se construye ([doc_template.hpp](../src/page/doc_template.hpp)): se renderiza la plantilla una vez con **marcadores** en lugar de los datos (un "esqueleto"), se parsea con el parser real, y de ese árbol salen los huecos y los bloques de repetición y de condición. Un valor escapado (`{{ x }}`) vive en un texto o en un atributo entrecomillado, donde el parser deshace el escapado, así que el hueco guarda la cadena cruda.

La plantilla **no tiene plan** (se traduce su HTML en cada petición, siempre correcto) si algo depende de los datos o de cómo lee el parser:
- un marcador en un nombre de etiqueta o de atributo, en un atributo sin comillas, en un comentario, en el contenido de `<script>`/`<style>`, en `<link href>`/`<script src>`, o en texto directamente bajo `#document`/`<html>`/`<head>`;
- un valor `|safe` (es HTML y el parser tendría que leerlo);
- un valor pegado a una entidad (`&{{ x }}`);
- un bloque que no abre y cierra bajo el mismo elemento, como un bucle que crea el `<tbody>` la primera vez o una etiqueta partida por un `{% if %}`;
- una URL relativa en la hoja de estilos (depende de la ruta que sirve la página).

**2. Traducir el HTML ya relleno.** Correcto para cualquier plantilla. La hoja de estilos y el programa se cachean por su texto fuente y se compilan una sola vez (la hoja se descarta si cambia un archivo de donde salió, comprobándolo como mucho una vez por segundo). Por petición queda parsear el HTML de la página y codificar su árbol.

**3. HTML.** Una página con JavaScript no tiene `.luxp` y se responde siempre en HTML (Luxium se la pasa a Chromium). Un `<script>` sin tipo que no compila como LuxScript se trata como JavaScript; un `<script type="text/luxscript">` que no compila es un error de compilación del proyecto.

- **Negociación.** Luxium envía `Accept: application/x-luxp, text/html;q=0.9`. La misma ruta y plantilla responden `application/x-luxp` a Luxium y `text/html` al resto, con `Vary: Accept`.
- **`render()` con datos constantes.** Se renderiza y se traduce al arrancar; servirlo es enviar bytes fijos.
- **Recursos enlazados.** `<link>`, `@import` y `url()` se resuelven con los montajes `static "/x" -> "./dir"` del bloque `app:`, como los pediría el navegador y sin salir de la carpeta montada.
- **Errores.** `lux --check` compila los `<script type="text/luxscript">` de las plantillas y marca el error en el `render()` con la línea de la plantilla.

**Comprobación de planes.** El documento que escribe un plan, decodificado y vuelto a escribir, tiene que ser byte a byte el de traducir el HTML relleno. Con `LUX_LUXP_CHECK=1` el servidor calcula cada respuesta también por el camino largo y las compara; si difieren, registra un error y envía la del camino largo. La suite de Lux lo activa y falla si aparece alguno. `LUX_LUXP_VERBOSE=1` explica en el log por qué una plantilla no tiene plan, y `LUX_LUXP_PLAN=0` desactiva los planes (para medir).

## Por qué no se traduce JavaScript

No es viable traducir JavaScript arbitrario a LuxScript:
- JavaScript tiene tipado dinámico, `eval`, prototipos, closures, `this` dinámico, un modelo asíncrono propio y una API DOM enorme. LuxScript, a propósito, no tiene nada de eso.
- Un traductor que solo cubriera "el subconjunto fácil" fallaría en silencio justo en las páginas reales.
- Además rompería la garantía del formato: que todo lo que ejecuta un `.luxp` pasó por el compilador con tipos y su capacidad es enumerable (`--inspect`).

Las páginas con JavaScript siguen yendo a Chromium.

## Estructura

```
.luxp   = "LUXP" 0x05 · páginas { nombre, documento, estilos, programa }* · recursos { nombre, bytes }*     (0 = entrada)
sección = blob: u8 zlib? · tamaño plano · datos
```

Los enteros son LEB128 y las cadenas llevan su longitud delante.

**Cada sección se guarda plana o comprimida por separado.** Así un servidor comprime una sola vez lo que nunca cambia (hoja de estilos y programa) y envía tal cual lo que sí cambia (el documento): la compresión no cuesta nada por petición. Un archivo escrito una vez (`lux pack`) las comprime todas con zlib nivel 9. El núcleo de Lux no enlaza zlib (la compresión es cosa del transporte): comprime solo si está compilado con su módulo `gzip`, y Luxium lee las dos variantes.

**Compacto a propósito** (los bytes de una visita):
- documento y estilos: los nombres que se repiten (etiquetas, atributos, propiedades CSS, selectores) se guardan una vez en una tabla y se referencian por índice; no se guardan números de línea;
- programa: tabla de cadenas común a todas las funciones, líneas como diferencias, sin los nombres de locales (la VM no los usa).

**Documento y estilos:**
- el árbol DOM, con `<script>`/`<style>` vacíos (siguen contando para `:nth-child`);
- las reglas ya parseadas, en orden de documento, con cada `@import` resuelto respecto a la hoja que lo contiene.

**Programa.** Las nativas viajan **por nombre**. El navegador las resuelve contra su propia `browser_api.inc`, así que un nombre que no conoce hace que se rechace la página. Un Lux y un Luxium de versiones distintas no pueden desincronizar ids en silencio.

## Garantías al abrir un `.luxp`

1. **Solo se ejecuta código verificado.** Nunca se compila código fuente, nunca se pasa a Chromium y nunca se lee fuera del paquete (un `../` no llega al disco).
2. **Cada sección se verifica al decodificarse.**
   - documento: límites de nodos y de profundidad (`dom::kMaxDepth` = 512, que ahora también aplica el parser HTML);
   - estilos: enumerados en rango, combinadores coherentes, `:nth` acotado;
   - programa: operandos en rango, nativas de la API del navegador, pila coherente, saltos condicionales hacia delante.
3. **Fuzzing.** [tools/luxpfuzz.py](../tools/luxpfuzz.py) corrompe bytes de `.luxp` generados por Lux y los abre con clicks en el navegador compilado con ASan: 0 crashes en 300 mutaciones.

## Capacidades: la base del "antivirus"

LuxScript no tiene `eval`, así que las nativas y métodos que aparecen en el programa son una cota superior exacta de lo que la página puede hacer (`luxium --inspect`). Siguientes pasos:
1. Política por capacidad: permitir, pedir permiso o rechazar.
2. Extraer los dominios cuando exista `fetch`.
3. Firma del payload.
4. Límites de memoria y de número de timers.

## Pendiente

- **Recursos de un `.luxp` servido por http** (imágenes): no viajan en la respuesta y Luxium todavía no carga imágenes por red.
- **`<script src="x.lux">` externo y `@font-face`.**
- **Valores CSS:** las declaraciones guardan su valor como texto, porque `var()` y las unidades relativas se resuelven por elemento.
- **Valores `|safe` y estructuras partidas** en las plantillas no tienen plan; si un caso común lo necesita, se puede ampliar el esqueleto.
- **Compresión del documento por petición:** hoy va plano; comprimirlo (zlib nivel 1) bajaría los bytes a costa de CPU por petición.
