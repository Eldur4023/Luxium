# Renderizado de Luxium: motor propio, oráculo y tests

Estado: el motor propio pasa **113/113** casos de `css-tests/` contra Chromium
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
| `src/style/stylesheet.*` | Parser de hojas: `!important`, `@media` (rango, `not`, listas), `@import`, `@supports/@layer` transparentes |
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
| `src/render/text.*` | FreeType + HarfBuzz + fontconfig: resolución de fuentes como Chromium, fallback por glifo |

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

Sin implementar (se ve como diferencia visual o se ignora): `transform`,
`position: sticky`, `::first-line/::first-letter`, contadores CSS, `outline`,
`text-shadow`, filtros y `mix-blend-mode`, `object-position`, multicolumna,
`writing-mode`, `@font-face` remotas, SVG, imágenes que no sean PNG (y sin
alfa), `<input>/<select>/<textarea>` (solo `<button>` básico), scroll interno
de cajas `overflow:auto`, `min()/max()/clamp()` mezclando `%` con `px`,
barra de scroll horizontal, `caption-side: bottom`.  Pendiente de método: importar un subconjunto de los reftests de web-platform-tests
(mismo formato que `ref/`, miles ya escritos).

## 4. Hacia "compilar todo en un archivo"

`ComputedStyle` es una struct tipada sin cadenas (salvo `url()` y `content`):
es el candidato natural a formato serializable.  Un compilador puede resolver
parseo, shorthands, unidades y `var()` estáticos en build y **rechazar** lo no
soportado con un diagnóstico (en vez de ignorarlo en silencio, como un
navegador), de modo que el subconjunto de CSS sea un contrato visible.  Lo que
debe seguir en runtime: `:hover`, `@media` dependiente del viewport y clases
cambiadas por LuxScript (la cascada ya opera sobre selectores compilados).
