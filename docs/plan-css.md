# plan-css.md — acercar el render propio a Chromium, por partes

Objetivo: que el **renderer propio** (sin CEF) pinta CSS tan parecido a
Chromium como sea razonable para un motor en espacio de usuario: cajas,
texto, color, imágenes y los dos grandes sistemas de layout modernos
(flexbox, grid), con un **criterio de parecido medible**, no a ojo.

Regla de oro: cada fase termina con pruebas comparadas contra Chromium.
Tenemos el oráculo perfecto en casa — CEF/Chromium off-screen (M6) puede
renderizar la misma página y ejecutarle JS. Nada de adivinar valores:
**se miden**.

---

## 0. Dónde estamos (inventario honesto)

Lo que ya existe:

- Parser CSS tolerante (`src/css/css.hpp`): reglas, compuestos
  (`div.card#main`), cadenas de descendientes, listas por comas,
  especificidad (id=100/class=10/tag=1), declaraciones como pares de
  strings; lo desconocido se salta sin morir.
- Cascade real: el look por defecto del navegador es **él mismo una hoja
  UA** (`kUAStylesheet` en `src/render/layout.cpp`) parseada con el mismo
  parser, así que una regla de página gana solo por especificidad.
- Propiedades: `color`, `background-color`, `font-size`, `font-weight`,
  `font-family` (detección mono), `line-height`, `text-align`,
  `display:none`, `margin*`, `padding*`; `style=""` inline; `<style>` y
  `<link>` locales.
- Layout monolítico (`src/render/layout.cpp`): bloques + palabras inline
  con wrap, botones/hr, scrollbar propia; texto con HarfBuzz/FreeType
  cacheado (`src/render/text.cpp`).

Lo que NO hay (y Chromium sí): valores computados tipados (todo son
strings resueltos al vuelo), modelo de caja fiel (margins colapsan,
min/max, box-sizing, %, calc), border/border-radius/box-shadow,
imágenes y gradientes, line boxes de verdad (leading, vertical-align),
overflow, z-index/stacking, position/float, tablas, flexbox, grid,
pseudo-elementos, atributos/nth-child, @media/@import, variables,
!important, herencia/inicial por tabla, font fallback.

Defecto estructural de fondo: **no hay separación entre estilo, cajas y
pintado**. `layout()` hace las tres cosas a la vez y devuelve shapes/words.
Todo el plan gira alrededor de arreglar eso primero.

---

## 1. Cómo lo hace Chromium (el mapa que imitamos, no el código)

Blink reduce el render a cinco etapas; nuestro objetivo es tener las
mismas costuras aunque cada una sea mil veces más simple:

1. **Parse** → hojas de reglas (ya lo tenemos).
2. **Style**: por elemento, resolver cascade → `ComputedStyle`: todos los
   valores TIPADOS y resueltos (longitudes en px, colores RGBA, enums),
   con herencia e `initial` por tabla de propiedades.
3. **Box tree**: cada elemento genera cajas (block/inline/…) que referencian
   su ComputedStyle.
4. **Layout**: algoritmos por tipo de caja producen un árbol de fragmentos
   con posiciones y tamaños en px.
5. **Paint**: recorre fragmentos en orden de pintado (CSS2.1 Appendix E +
   stacking contexts) y emite primitivas.

Hoy nuestro layout() mezcla 2+3+4. El plan introduce ComputedStyle (fase 2)
y un árbol de cajas (fase 3) para que cada algoritmo posterior (tablas,
flex, grid) tenga donde engancharse sin reescribir todo cada vez.

---

## 2. Fase 0 — El oráculo: comparar contra Chromium de verdad

**Por qué primero**: sin esto, "parecido a Chromium" es una opinión. Con
esto, es un porcentaje.

Trabajo:

- `css-tests/`: corpus de páginas mínimas, una feature por página
  (`box-margin.html`, `flex-center.html`, `table-collapse.html`…), cada una
  con un `data-expected` opcional.
- Herramienta `luxium --probe <url>`: con CEF en modo OSR (sin ventana),
  carga la página y ejecuta por `CefFrame::ExecuteJavaScript` un script que
  recorre el DOM y vuelca JSON:
  - por elemento: tag, `getBoundingClientRect()` (x/y/w/h redondeados),
    `getComputedStyle()` de las propiedades que nos importan;
  - y un screenshot del frame (OnPaint ya lo tenemos → PNG).
- Herramienta `luxium --cssdiff <url>`: renderiza la MISMA página con el
  motor propio al mismo ancho y compara:
  - **numérico**: rects propios vs rects del oráculo (tolerancia ±1px);
  - **píxel**: diff de imágenes con mapa de calor por región.
- Volcado de nuestro layout en el mismo formato JSON (posiciones de cajas),
  para diff directo sin OCR de píxeles.
- Medir con el oráculo los **defaults de Blink** (h1–h6, ul/ol/li, p,
  body, a, strong/em…) y regenerar `kUAStylesheet` con esos valores
  exactos en vez de los actuales a ojo.

Aceptación:
- `--cssdiff css-tests/cajas.html` lista cada caja desalineada con
  esperado/obtenido.
- UA stylesheet regenerada: en una página sin CSS propio, h1/p/ul difieren
  de Chromium en 0px.

El catálogo COMPLETO de pruebas (ids, qué comprueba cada una, tolerancias
y veredictos) está definido desde ya en `docs/plan-css-tests.md`: cada
fase de este plan nace con sus tests ya enumerados.

No en esta fase: automatizar en CI; comparar fuentes a nivel de glifo.

---

## 3. Fase 1 — ComputedStyle: valores tipados y cascade completa de valores

**Por qué**: hoy cada consumidor re-parsea el string de la declaración
("14px", "50%", "red"). Sin valores tipados no hay herencia correcta, ni
unidades, ni shorthand, ni var(). Es la viga maestra.

Trabajo:

- `struct ComputedStyle` en `src/css/`: una entrada por propiedad con tipo
  real (Length{px|percent|auto}, Color{r,g,b,a}, enums, fonts…).
- **Tabla de propiedades**: initial value, hereda sí/no, cómo se anima
  (nos da igual, pero la tabla centraliza). Herencia = copiar campos.
- Parser de valores: px, em, rem, %, pt, ch, vw/vh, 0 sin unidad; colores
  named/hex/rgb()/rgba()/hsl(); keywords `inherit|initial|unset|revert`.
- Shorthands → longhands (`margin` → 4, `padding` → 4, `border` →
  width/style/color por lado, `background` → color/image/repeat/position,
  `font` mínimo).
- `!important` (ahora se ignora): author > UA, inline > hoja, !important
  invierte el orden entre orígenes, como la spec.
- **Variables**: `--x` en custom properties (heredan, se guardan como
  token stream) y `var(--x, fallback)` resuelto en tiempo computed. Es lo
  que más usan las páginas modernas y barato de resolver aquí.
- `calc()` básico: `+ - * /` con mezcla px/% (resolución de % queda para
  layout, se conserva simbólicamente).
- Cascade: recopilar declaraciones ganadoras por (propiedad, origen,
  especificidad, orden) y producir UN ComputedStyle por elemento, cacheado
  e invalidado como hoy (node → dirty).

Aceptación:
- Suite `css-tests/values/*.html`: cada caso imprime valores y el diff
  contra `--probe` da 0 diferencias en las propiedades soportadas.
- Una hoja con variables y calc mezclando px/% resuelve igual que Chrome
  (donde el % depende del ancho del contenedor, se compara tras layout).

No en esta fase: heredadas raras (white-space se mete en fase de texto),
`@property`, Houdini, animaciones.

---

## 4. Fase 2 — La caja fiel: block layout según CSS 2 (con Chromium al lado)

**Por qué**: el 80% del "se ve raro" de páginas simples es modelo de caja.

Trabajo:

- Árbol de cajas explícito (`BlockBox`, `InlineBox` mínimo) que referencia
  ComputedStyle; layout produce fragmentos (x/y/w/h) que el pintado actual
  consume. Los shapes/words actuales se vuelven salida de pintado, no de
  layout.
- width/height: auto, px, %, min/max-width/height, `box-sizing`
  (content-box | border-box).
- Margins: **colapso de márgenes verticales** entre hermanos y
  padre/primer-hijo (las reglas de la spec: positivos se suman,
  negativos el más negativo, etc. — es EL bug visual clásico).
- `border` (width/style/color por lado; solo `solid`/`none` en esta fase),
  `border-radius` (esquinas, con clipping al pintar).
- Padding/width en % resueltos contra el containing block correcto.
- line-height numérico (1.5) y con unidad; altura de línea de bloques.
- UA stylesheet: mover los tamaños de h1–h6/listas a los valores Blink.

Aceptación:
- `css-tests/box/*.html` (margin-collapse, percent, border-box, minmax):
  diff de rects = 0 contra el oráculo a ancho fijo.
- Una página "blog simple" (títulos, párrafos, listas, cajas con borde) con
  pixel-diff < umbral acordado y sin diferencias estructurales.

No en esta fase: floats, position, overflow (van en fases 5/6), tablas.

---

## 5. Fase 3 — Línea a línea: inline de verdad y texto

**Por qué**: el texto es la mitad del píxel de cualquier página.

Trabajo:

- **Line boxes** reales: leading repartido (half-leading), baseline común
  por línea, `vertical-align` (baseline/sub/super/middle + px/%),
  alturas por fragmento inline (inline-block básico).
- Procesado de espacios estándar: colapso, saltos, `white-space`
  (normal/nowrap/pre/pre-wrap), `<br>`.
- `text-transform`, `letter-spacing` (ya hay), `word-spacing`,
  `text-decoration` (underline/line-through, color y grosor razonables).
- `text-overflow: ellipsis` + `overflow-wrap`/`word-break` (ruptura de
  palabras largas), `text-align` (justify incluido).
- Ruptura de líneas: UAX#14 simplificado (no partir donde no se debe:
  puntuación, guiones).
- Fallback de fuentes por carácter vía fontconfig (que un chino o un
  árabe no salga en tofu si hay fuente instalada).

Aceptación:
- Suites de line-boxes comparadas por rect de cada línea (el oráculo puede
  dar los rects de spans).
- Página con párrafos justificados + nowrap + ellipsis: mismas roturas de
  línea que Chromium en el corpus (o documentadas si HarfBuzz difiere).

No en esta fase: shaping complejo por scripts (árabe completo, indic),
font-feature-settings, subpixel.

---

## 6. Fase 4 — Pintado: orden, capas y lo visual moderno

**Por qué**: aquí vive el "look" (sombras, radios, imágenes) y el orden
correcto de solape.

Trabajo:

- Orden de pintado CSS2.1 Appendix E (versión honesta): background del
  bloque → bordes → descendientes inline → … con **stacking contexts**
  simplificados (z-index por elemento posicionado).
- `background` completo: color + `background-image: url()` (PNG/JPEG con
  stb_image), `repeat`, `position`, `size` (cover/contain/px/%),
  **gradientes** `linear-gradient()`/`radial-gradient()` básicos.
- `box-shadow` (outer, inset opcional), `outline`, `opacity` (compositing
  por capa temporal), `border-radius` clipping real (máscaras por scanline).
- `overflow`: visible/hidden/scroll/auto con scrollbar propia integrada al
  layout (altura del viewport del sub-árbol).
- `<img>`: dimensiones intrínsecas, `width/height`, `object-fit`
  (fill/cover/contain), `alt` como texto si no carga; imágenes por red con
  el fetch existente.
- `visibility`, `cursor` (ya hay hand), `pointer-events: none`.

Aceptación:
- Suite visual `css-tests/paint/*.html`: pixel-diff contra el frame del
  oráculo con mapa de calor; umbral por página acordado (anti-aliasing
  perdonará ruido fino, no formas).
- Una "card" moderna (radius + shadow + imagen cover + gradiente) clava el
  diff estructural.

No en esta fase: filtros, blend modes, transforms, will-change, capas GPU.

---

## 7. Fase 5 — Selectores de verdad

**Por qué**: las hojas reales usan esto a diario; el parser ya tolera, hay
que EVALUAR.

Trabajo:

- Combinadores `>` (hijo), `+` (hermano adyacente), `~` (hermanos).
- Atributos: `[attr]`, `[attr=v]`, `^= $= *= ~= |=`.
- Estructurales: `:first-child`, `:last-child`, `:only-child`,
  `:nth-child(an+b)` / `:nth-of-type`, `:not()`.
- Dinámicas que ya existen a medias: `:hover` (recalcular estilo del
  sub-árbol afectado), `:active`.
- Pseudo-elementos: `::before`/`::after` con `content` (string, `attr()`,
  contador básico si toca), heredando estilo del origen.
- `@import` (recursivo, con ciclo-guard), `@media` (screen/width, ya que el
  viewport es fijo lo aplicamos al cargar y al redimensionar).
- Match del motor: de matching O(n·m) por regla a índices por tag/class/id
  (Bloom/hoists tipo Blink) si el perfil lo pide.

Aceptación:
- Suite de selectores: para cada selector, una página con N elementos
  marcados; diff contra oráculo de qué elementos reciben estilo (por
  color canario) = 0 errores.
- Reset moderno (`*:where()` no; `box-sizing` en `*` con `*, *::before`)
  funciona.

No en esta fase: `:has()`, `:is()/:where()` (baratos después),
namespaces, `@supports`.

---

## 8. Fase 6 — Tablas

**Por qué**: correos, docs y media wiki viven de tablas; además el layout
de tabla es contenido (auto) y da pelea.

Trabajo: display table/row/cell (y anónimas cuando falta el envoltorio),
colspan/rowspan del parser HTML, anchos por algoritmo auto (min/max
content por celda, reparto proporcional), `border-collapse`
(separate|collapse), `vertical-align` de celda, `<th>` negrita centrada
en la UA.

Aceptación: suite de tablas (auto widths, collapse, spans) con diff de
rects 0 contra oráculo a ancho fijo; la página de ejemplo `tabla.html`
del corpus pinta igual.

No en esta fase: fixed layout, `table-caption`, group columns.

---

## 9. Fase 7 — Position y floats

**Por qué**: todo lo anterior es flujo; aquí empieza el solape real.

Trabajo:

- `position: relative` (offset sin sacar del flujo), `absolute`
  (containing block = ancestro posicionado; coordenadas y % contra él),
  `fixed` (viewport); `inset` (top/right/bottom/left, shorthands lógicos
  fuera de alcance).
- `float: left/right` + `clear`: modelo de line boxes rodeando floats
  (es el punto de más bugs; el corpus de pruebas manda).
- `z-index` + orden de pintado (apoyado en la fase de pintado).
- `position: sticky` (opcional, si el scroll propio lo permite barato).

Aceptación: suites clásicas (float-wrap, absolute-in-relative, fixed
bar) con rects exactos y pixel-diff; overlay/dropdown de la página demo
funciona.

No en esta fase: `float` dentro de flex/grid (la spec lo prohíbe para
hijos flex/grid: ni lo modelamos), multicolumna.

---

## 10. Fase 8 — Flexbox (nivel 1)

**Por qué**: es el layout de la web moderna; sin esto nada "app-like"
encaja.

Trabajo:

- Algoritmo de la spec por fases (single-line primero): flex-basis →
  grow/shrink con min/max → eje cruzado con align-items/align-self →
  `gap`, `order`, `justify-content` completo, `flex-wrap` (multi-línea
  después del single-line) y `align-content`.
- `flex` shorthand → grow/shrink/basis; `auto/none/initial`.
- Los ítems son cajas del árbol (fase 2): su layout reusa el block/inline
  existente.

Aceptación: la suite clásica de casos (navbar, cards en fila con
wrap, center perfecto, sidebars con basis fijo) con rects = oráculo;
pixel-diff de una "app shell" de juguete.

No en esta fase: contenedores anidados profundos con aspect-ratio,
`::first-line`, interacción grid+flex más allá de la spec trivial.

---

## 11. Fase 9 — Grid (explícito, sin ambiciones)

Trabajo: `grid-template-columns/rows` (fr, px, %, auto, repeat()),
`gap`, auto-placement por fila (dense fuera de alcance),
`grid-column/row` con líneas explícitas, `align/justify-items/content`.
Nada de subgrid, ni names areas (quizá `grid-template-areas` si sobra
tiempo: es barato sobre lo anterior).

Aceptación: 6–8 páginas de suite con rects = oráculo.

---

## 12. Orden recomendado y por qué

0. Oráculo (todo lo demás se mide con él) → 1. ComputedStyle → 2. Caja →
3. Texto/inline → 4. Pintado → 5. Selectores → 6. Tablas → 7.
Position/floats → 8. Flexbox → 9. Grid.

La lógica: (a) medir antes de construir; (b) valores tipados y árbol de
cajas son prerequisito de TODO algoritmo posterior; (c) tablas y flex son
algoritmos que "enchufan" al árbol de cajas una vez existe; (d) flex va
después de position/floats porque la spec de flex elimina floats de sus
hijos y simplifica decidir qué se implementa dónde.

Impacto esperado por esfuerzo: tras la fase 4, páginas "documentos" se ven
prácticamente clavadas; tras la 8, la web "app" sencilla entra.

## 13. Riesgos

- **Margin collapsing y floats** son los dos agujeros de conejo clásicos:
  el corpus de pruebas va DELANTE del código en esas áreas.
- ComputedStyle cacheado + hover → invalidación fina; empezar grosero
  (sub-árbol) y medir.
- Rendimiento del pintado full-frame por daño: hoy va sobrado a 120fps con
  páginas pequeñas; con imágenes/gradientes habrá que introducir daño por
  región (la infraestructura de shapes ya separa capas: es extensible).
- HarfBuzz/FreeType vs Skia: el kerning/shaping coincidirá; el
  anti-aliasing no píxel a píxel — los umbrales de pixel-diff lo asumen.

## 14. No-goals (explícitos, en todo el plan)

Animaciones/transitions, transforms (2D/3D), filtros y blend modes,
`@font-face` con formatos web (solo TTF locales si se da), shadow DOM,
container queries, print layout, subgrid, SVG, web fonts remotos,
`@property`, contadores CSS completos, writing modes verticales.
