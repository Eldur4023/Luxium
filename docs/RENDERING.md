# Renderizado de Luxium: motor propio, oráculo y tests

Estado: el motor propio pasa **122/122** casos de `css-tests/` (más 3 instantáneas de Homeflix que fallan en detalles conocidos, §3) contra Chromium
(rects ±1px, estilos computados exactos, píxeles, color).  Este documento
sustituye a `plan-css.md` / `plan-css-tests.md` (que quedan como histórico).

## 1. El motor

```
DOM ──► style/ ──► layout/build ──► layout/{block,inline,flex,grid,table} ──► layout/paint ──► paint/raster
        cascada     árbol de cajas    geometría (px)                          display list     píxeles
```

| Módulo | Qué hace |
|---|---|
| `src/style/selector.*` | Selectores L3 (+ `:is/:where/:not`, `nth-*`, `[attr op v i]`, `::before/::after`), especificidad (a,b,c) |
| `src/style/stylesheet.*` | Parser de hojas: `!important`, `@media` (rango, `not`, listas), `@import`, `@supports` transparente, `@layer` (orden de capas en la cascada) |
| `src/style/values.*` | Longitudes (`calc()`, `em/rem/vw/pt…`), colores (148 nombres, `hsl()`, `#rgba`), degradados, sombras |
| `src/style/properties.*` | Tabla de propiedades → `ComputedStyle` tipado; shorthands; `inherit/initial/unset` |
| `src/style/cascade.*` | Orden origen/`!important`/especificidad, `var()`, atributos presentacionales (`bgcolor`, `<table border>`…), herencia |
| `src/layout/build.cpp` | DOM + estilos → cajas; contenido generado, marcadores de lista, cajas anónimas (inline/bloque, items flex/grid, estructura de tabla) |
| `src/layout/block.cpp` | Anchos/alturas, **colapso de márgenes**, floats, `position`, replaced, tamaños intrínsecos |
| `src/layout/inline.cpp` | Tokenización (espacios, saltos), líneas alrededor de floats, strut/half-leading, `vertical-align`, `text-align`, elipsis |
| `src/layout/flex.cpp` | Algoritmo flexbox de la spec (base size, líneas, congelado, `align-*`, `justify-content`, `gap`, reverse) |
| `src/layout/grid.cpp` | Grid: colocación explícita/auto (sparse/dense), áreas, `fr/minmax/auto-fill`, alineación |
| `src/layout/table.cpp` | Tablas: auto/colspan/rowspan, `border-spacing`, `border-collapse` con conflictos de bordes |
| `src/layout/paint.cpp` | Posiciones absolutas, Apéndice E de CSS 2.1 con contextos de apilamiento, fondos/bordes/sombras/texto |
| `src/paint/raster.*` | Rasterizador software: SDF con AA, clip redondeado, capas de opacidad, texto LCD |
| `src/render/text.*` | FreeType + HarfBuzz + fontconfig: resolución de fuentes como Chromium, fallback por cluster (secuencias emoji: VS16, ZWJ, tonos, banderas, keycaps), emoji a color (tiras CBDT escaladas con filtro de caja y avance entero de la tira) |
| `src/render/svg.*` | Rasterizador SVG propio (AA por supermuestreo vertical + cobertura horizontal exacta): paths (todos los comandos y arcos), formas, fill/stroke (caps, joins, dash), opacidad, `<g>/<use>`, transformaciones, degradados lineal/radial, `viewBox`/`preserveAspectRatio`, `<style>` mínimo. Se re-rasteriza al tamaño con que se coloca (fondo, `<img>`, `<svg>` inline) |
| `src/shell/shell_anim.cpp` | `transition` y `@keyframes`/`animation` (reloj del bucle de frames; solo repinta mientras algo anima) |

Fuentes: `font-family` pasa por fontconfig y solo se acepta la familia pedida o
una **métricamente compatible** (misma regla que Skia: Arial/Arimo/Liberation
Sans, Times New Roman/Tinos/Liberation Serif, Courier New/Cousine/Liberation
Mono, Cambria/Caladea, Calibri/Carlito; `Helvetica` cuenta como Arial).
Genéricos: `serif`→Times New Roman, `sans-serif`→Arial, `monospace`→la fija del
sistema, `system-ui`→la sans por defecto.  Sin `font-family`: Times New Roman,
como Chromium.  Texto: hinting ligero vertical, AA LCD (filtro FIR de 5 taps),
posición de glifo en cuartos de píxel y curva de cobertura `c^0.87`
(calibrado contra la salida de Chromium en esta máquina).

## 2. Los tests

Cada página de `css-tests/` se renderiza **dos veces** y se compara:

1. **Oráculo** (Chromium vía CEF off-screen): `luxium page --cefshot x.ppm --cefprobe x.json`
   guarda el frame y, por cada `[data-t]`, `getBoundingClientRect()` y
   `getComputedStyle()` (lista de propiedades en `src/style/dump.cpp`).
2. **Motor propio**: `luxium page --shot x.ppm --boxes b.json` y
   `luxium page --styles s.json` (estilos sin ventana).

`tools/cssrun.py` da el veredicto; un caso pasa solo si **todo** se cumple:

- rects de cajas por `data-t` dentro de ±1px (veredicto principal);
- estilos computados iguales (colores/enums exactos, longitudes ±0.5px);
- píxeles: % distinto bajo el umbral, sin regiones sólidas, sin bloques de tinta
  o de color distintos (el color atrapa el "texto gris en vez de azul").

```sh
python3 tools/cssrun.py                    # toda la suite
python3 tools/cssrun.py -v flex grid       # detalle por elemento
python3 tools/cssrun.py --check-baseline   # exit 1 si algo de BASELINE.txt deja de pasar
python3 tools/cssrun.py --write-baseline   # tras mejorar
python3 tools/cssgolden.py --force dir     # regenerar goldens (necesita DISPLAY=:0)
```

Opciones por test, en un comentario de la página:
`<!-- cssrun: sizes=1180x820,800x700,420x700 threshold=3 click=a,b -->`.

- `sizes=`: el mismo test en varios viewports (goldens `id@WxH.*`).
- `click=`: ids que se pulsan (LuxScript) antes del shot; el golden sale de
  `name.expected.html`, la página estática equivalente al estado final.
- `threshold=`: % de píxeles permitido; úsalo solo con motivo escrito.
- `skipstyle=`: propiedades que no se comparan como estilo computado (las que anima `@keyframes`: `--styles` no tiene reloj de animación; se comprueban por píxeles y rects).

**Reftests** (`css-tests/ref/`, `tools/cssref.py`): `X.html` y `X-ref.html` dicen lo
mismo de dos maneras (margin:auto vs margen explícito, grid vs flex, `calc()` vs literal,
`:nth-child` vs clases…) y deben pintar *idénticos* con el motor propio; no interviene
Chromium.  Hay 12 pares; añadir uno es copiar dos ficheros.

Regla de proceso: **un commit no dice "feature X" si los tests de X no pasan**.
`.githooks/pre-commit` ejecuta `--check-baseline` cuando el commit toca el motor
(`git config core.hooksPath .githooks` para activarlo).

Directorios: `box/ text/ values/ sel/ flex/ grid/ table/ pos/ paint/` (una
característica por página), `integration/` y `real/` (páginas tipo web real),
`resp/` (varios anchos, `@media`), `dyn/` (mutación del DOM desde LuxScript),
`self/` (valida el propio harness).

## 3. Lo que aún no hay

Hecho en el frente "formularios y SPA" (workstream B, plan-luxscript-spa.md): controles `<input>` (text, password,
search, number, range, checkbox, radio, file), `<textarea>`, `<select>` con popup, `<label>`; foco y orden de tabulación;
caret/selección/portapapeles/IME; `:hover :focus :focus-visible :focus-within :checked :disabled :has() :placeholder-shown
::placeholder`; `cursor`; `outline`; `accent-color`; `appearance`; `aspect-ratio`; `min()/max()/clamp()` mezclando `%` y px;
`dvh/svh/lvh`; `overflow: auto/scroll` con rueda, barras y arrastre; `position: sticky` (contra el scroll de página);
`transform` 2D completo (translate/scale/rotate/skew/matrix + `transform-origin`, en pintura, hit-testing y `getBoundingClientRect`;
una traslación pura se encaja a píxeles enteros, lo demás se pinta en una capa remuestreada); `transition` (shorthand y longhands) de
opacity/transform/colores/longitudes (width, height, margin, padding, inset); `@keyframes` + `animation` (shorthand y longhands, `cubic-bezier`/`steps`/
palabras clave, `infinite`, direcciones, `fill-mode`, `play-state`, varias animaciones por elemento, también en `::before/::after`, `var()` en los
fotogramas) sobre esas mismas propiedades; SVG (§1); emoji a color y fallback monocromo por cluster; `<details>`; imágenes JPEG y WebP; esquema oscuro
(`<meta name=color-scheme content=dark>`).

Animaciones: el reloj de `@keyframes` es el del bucle de frames; `LUXIUM_ANIM_TIME=ms` lo congela en ese instante desde el
inicio de cada animación (volcados deterministas).  Los tests usan `animation-play-state: paused` con retraso negativo, que Chromium
y el motor propio muestran igual; `cssrun` admite `skipstyle=` (las propiedades que anima `@keyframes` no se comparan como estilo
computado, `--styles` no tiene reloj).  Cada frame de una animación hace relayout completo (como las transiciones).  Las
transformaciones no 3D se interpolan función a función (mismo número y tipo de funciones; si no, salto a mitad); `perspective`,
`rotateX/Y`, `translateZ` se ignoran.  SVG: sin `clip-path`/`mask`/`filter`/`pattern`/`<text>`/`<image>`/`marker` ni `<symbol>` con `viewBox`.
Emoji: solo la tira CBDT de Noto Color Emoji (sin COLRv1/sbix), con el mismo orden de fontconfig que Chromium para elegir la fuente.

Sin implementar o distinto de Chromium (medido con `css-tests/probe/`, 22 páginas contra el oráculo): `writing-mode` vertical, transformaciones 3D (`perspective`, `rotateX/Y`, `translateZ`), `shape-outside`, `font-variant` (small-caps) y `font-feature-settings`, `<svg>` sin `pattern`/`<text>`/`<image>`/`marker`, barra de scroll horizontal arrastrable, barras de cajas internas
como *overlay* (no reservan hueco, a diferencia de Chromium), PNG entrelazado/16 bits.
Ya implementados y en la suite: multicolumna, `::first-letter`/`::first-line`, `subgrid`, `mix-blend-mode` y `background-blend-mode`, `@layer`, radio elíptico, `border-image`, bidi a nivel de palabra con `direction: rtl` (sin espejar flex/tablas/flotantes), `auto-fit` que colapsa pistas vacías, hueco de scrollbar clásico (15px) en `overflow: auto|scroll`, outlines sobre el resto del contexto, `text-shadow`, filtros, `backdrop-filter`, `mask-image`, `clip-path`, contadores, `object-position`,
`@font-face`, `cq*`, `text-decoration` wavy/dotted/double, gradientes cónicos y repetidos, `object-fit`, `aspect-ratio`, `@supports`, `:is/:where`.
Pendiente de método: importar un subconjunto de los reftests de web-platform-tests.

## 3b. Formularios, foco y eventos (para el runtime de scripts)

Un solo punto de salida: **`Page::dispatch(kind, node, EventData)`** (page.hpp).  Pasa por `Page::event_tap` (observador:
`luxium --log-events` lo imprime) y luego por `Engine::dispatch` (LuxScript).  Devuelve `true` si un handler llamó
`event.prevent()`.  Tipos que dispara el shell/formularios: `input` (EventData.key = texto insertado, .value = valor
nuevo), `change`, `submit`, `focus`, `blur`, `keydown`/`keyup` (vía `Page::key_event`; una sola vez por tecla), `click`,
`mouseenter`/`mouseleave`/`mousemove` (solo si alguien escucha: `Page::wants_event`), `wheel` (EventData.delta; prevent =
el script se queda la rueda), `scroll` (EventData.x/.y = nuevo offset, en cajas con overflow), `visibilitychange`.

Estado de los controles: vive en el DOM (atributos `value`/`checked`/`selected`, hijo de texto del `<textarea>`), así que
`:checked`, `:placeholder-shown` y el layout ven lo mismo que los scripts.  API en `Page`: `form_value/set_form_value`,
`form_checked/set_form_checked`, `select_index/select_option`, `form_files/set_form_files`, `focus/blur/focused/focus_next`,
`edit_*` (inserción, borrado, caret, selección; cada una dispara `input`), `activate` (clic: etiqueta → control, toggles).

`ShellHooks` (script/engine.hpp) rellenados por `Shell::wire_form_hooks()`: `rect`, `scroll_get/set/into_view`, `focus`,
`focused`, `get_value/set_value` (valor vivo del modelo de formularios), `files` (rutas elegidas con zenity; `LUXIUM_ZENITY`
cambia el binario en tests).  El texto se edita desde SDL_TEXTINPUT/SDL_TEXTEDITING (IME: preedit subrayado en el caret);
`SDL_StartTextInput` solo se activa en la ventana con foco de teclado, también para las ventanas de apps del host.

Pruebas sin pantalla: `tools/formtest.py` (scripts de entrada sintética `--script`, líneas `ms click|down|up|move|text|key|wheel|edit`)
y la suite `css-tests/forms/` (tamaños de controles contra Chromium).  `tools/homeflixsnap.py` regenera `css-tests/homeflix/`
(instantáneas del DOM real de cada vista de Homeflix servida con datos de prueba + goldens de Chromium).

Rendimiento: `LUXIUM_TIMING=1` imprime build/layout/paint y repaint.  Catálogo de 500 tarjetas (`examples/perf/catalog-500.html`):
relayout ≈ 9.5 ms (estilo 2.9 + layout 3.6 + paint 0.8, una pasada) y repaint ≈ 7 ms; la primera carga hace dos pasadas por la
barra de scroll.  El layout sigue siendo completo por mutación (no incremental); lo compensan el índice de reglas y la caché de
estilos por (reglas que casan, estilo del padre, etiqueta).

## 4. Hacia "compilar todo en un archivo"

`ComputedStyle` es una struct tipada sin cadenas (salvo `url()` y `content`):
es el candidato natural a formato serializable.  Un compilador puede resolver
parseo, shorthands, unidades y `var()` estáticos en build y **rechazar** lo no
soportado con un diagnóstico (en vez de ignorarlo en silencio, como un
navegador), de modo que el subconjunto de CSS sea un contrato visible.  Lo que
debe seguir en runtime: `:hover`, `@media` dependiente del viewport y clases
cambiadas por LuxScript (la cascada ya opera sobre selectores compilados).
