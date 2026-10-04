# plan-css-tests.md — el catálogo de pruebas CSS (definido desde el día 0)

Complemento de `docs/plan-css.md`: AQUÍ están los tests. Se definen antes
de escribir código de layout, así cada fase nace con su vara de medir.

---

## 1. Mecánica: cómo se ejecuta un test

Cada test es una página HTML autocontenida en `css-tests/`. El mismo archivo
se renderiza DOS veces y se compara:

1. **Oráculo**: `luxium --probe <file>` — CEF off-screen (M6) carga la
   página y ejecuta por `ExecuteJavaScript` un sondeo que vuelca JSON:
   - por elemento con atributo `data-t` (marcamos solo lo que importa):
     tag, `id`, `getBoundingClientRect()` redondeado a entero,
     y `getComputedStyle()` de las propiedades en la lista fija
     (`color, background-color, font-size, line-height, margin*,
     padding*, border*, width, height, display, position, text-align,
     font-weight, ...`);
   - el frame completo como PNG (OnPaint → PNG, ya lo tenemos).
2. **Nosotros**: el motor propio renderiza al mismo tamaño (1180x820, la
   ventana estándar) y vuelca el MISMO formato JSON desde el árbol de
   fragmentos + un frame PNG.

Comparación (`luxium --cssdiff <file>`):

- **rects**: esperado vs obtenido por `data-t`; tolerancia **±1px** por
  coordenada. Diferencia estructural = caja que falta, sobra o está a
  más de 1px.
- **computed**: colores y enums EXACTOS; longitudes ±1px.
- **píxel**: porcentaje de píxeles distintos (anti-aliasing perdonado:
  un píxel cuenta como distinto si la diferencia de luminancia > 24).
- Salida: una línea por test `PASS|FAIL <id> (<n diffs>)` y en FAIL el
  detalle esperado/obtenido. Exit 0 solo si todo pasa.
- `luxium --cssdiff-all css-tests/` recorre el árbol y resume
  (útil como regresión perpetua, hermano de `--check`).

## 2. Convenciones de las páginas de test

- **Una feature por página.** Si un test falla, el fallo debe apuntar a
  una regla concreta.
- Fuentes fijas de parte a parte: `font-family: "DejaVu Sans"` (la que ya
  empaqueta Luxium; Chromium la tiene vía fontconfig). Nada de webfonts.
- Sin red, sin JS de página, sin animaciones; estático y determinista.
- Cada elemento medido lleva `data-t="nombre-unico"`.
- Ancho fijo de layout: 1180px (si un test necesita otro, lo dice en
  `data-width` y el harness lo respeta en ambos motores).
- Los tests de PÍXEL usan PNG generados por nosotros (patrón determinista
  de rayas/cuadros), nunca fotos: el diff debe ser interpretable.

---

## 3. El catálogo

IDs estables `prefijo-nombre`; el prefijo dice la fase que los habilita.
(Marcados con ◆ los que bloquean su fase: sin ellos, no se cierra.)

### 3.1 Harness se valida a sí mismo (antes de nada)

| id | qué comprueba |
|----|---------------|
| `t-self-static` | página sin CSS: rects de h1/p/ul idénticos (valida UA + sonda) |
| `t-self-utf8` | texto con acentos/emoji placeholder: el sondeo no corrompe |
| `t-self-overflow` | página más alta que el viewport: scroll no altera rects de vista |

### 3.2 Fase 1 — ComputedStyle (`values-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `values-color-forms` | named/hex3/hex6/rgb()/rgba()/hsl() → mismo RGBA |
| ◆ `values-units` | px em rem % pt ch vw vh sobre width/font-size |
| ◆ `values-shorthand-spacing` | margin/padding con 1,2,3,4 valores |
| ◆ `values-shorthand-border` | border → width/style/color por lado |
| `values-shorthand-background` | color+repeat+position descompuestos |
| ◆ `values-important` | !important de página gana a inline sin important |
| ◆ `values-keywords` | inherit/initial/unset por propiedad heredable y no |
| ◆ `values-var-basic` | --x + var(--x) |
| `values-var-inherit` | custom property heredada al sub-árbol |
| `values-var-fallback` | var(--inexistente, fallback) |
| `values-calc` | calc(100% - 40px), calc mezclando px y % |

### 3.3 Fase 2 — Modelo de caja (`box-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `box-margin-collapse-siblings` | p+p: el hueco es max(m), no m+m |
| ◆ `box-margin-collapse-parent` | margin-top del primer hijo sale del padre |
| ◆ `box-margin-collapse-negative` | positivos y negativos mezclados |
| ◆ `box-width-percent` | % contra el content box del padre |
| ◆ `box-sizing` | content-box vs border-box con border+padding |
| ◆ `box-minmax` | min/max-width pisan a width y a auto |
| `box-height-auto` | altura por contenido, hijos en flujo |
| `box-border-content` | el border empuja el contenido en content-box |
| ◆ `box-radius-paint` | border-radius: silueta y clipping (píxel) |
| `box-auto-centering` | margin: 0 auto centra el bloque |
| `box-padding-percent` | padding % contra ANCHO del padre (también vertical) |
| `box-ua-defaults` | h1–h6/p/ul/li sin CSS propio: rects = Blink |

### 3.4 Fase 3 — Texto e inline (`text-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `text-line-boxes` | alturas de línea con line-height 1 / 1.5 / 24px |
| ◆ `text-wrap-identical` | párrafo largo: MISMAS roturas de línea (rects de spans) |
| ◆ `text-white-space` | normal / nowrap / pre / pre-wrap |
| `text-align-justify` | justificado: anchos de línea iguales |
| `text-ellipsis` | text-overflow en bloque de ancho fijo |
| `text-overflow-wrap` | palabra kilométrica se rompe igual |
| `text-decoration` | underline/line-through: posición y color |
| `text-transform` | uppercase/lowercase/capitalize (rects no cambian de sitio) |
| `text-spacing` | letter-spacing + word-spacing acumulados |
| `text-vertical-align` | baseline/sub/middle + px en la misma línea |
| `text-em-strong` | inline anidado: el rect del fragmento bold |

### 3.5 Fase 4 — Pintado (`paint-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `paint-order-overlap` | dos bloques solapados: qué color gana y dónde |
| ◆ `paint-bg-image` | PNG patrón: repeat x/y, position, size cover/contain |
| ◆ `paint-gradient-linear` | 2 y 3 paradas, ángulos 0/90/45 (píxel, umbral) |
| `paint-gradient-radial` | circle/ellipse básicos (píxel, umbral) |
| ◆ `paint-box-shadow` | sombra exterior: geometría del halo (píxel) |
| ◆ `paint-overflow-hidden` | clip real del sub-árbol |
| `paint-overflow-auto` | alto de scroll del sub-árbol + scrollbar propia |
| `paint-opacity` | grupo al 50% sobre fondo conocido (píxel) |
| `paint-zindex` | z-index entre hermanos y entre stacking contexts |
| ◆ `paint-img-intrinsic` | PNG conocido: tamaño natural y forzado |
| `paint-object-fit` | cover/contain/fill sobre marco fijo |
| `paint-visibility` | visibility:hidden reserva espacio, display:none no |

### 3.6 Fase 5 — Selectores (`sel-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `sel-combinators` | > + ~ descendiente, encadenados |
| ◆ `sel-attr` | [a] [a=v] ^= $= *= ~= \|= |
| ◆ `sel-nth` | nth-child(an+b), first/last/only, nth-of-type |
| ◆ `sel-not` | :not(cadena simple) |
| ◆ `sel-before-after` | ::before/::after con content y estilos propios |
| `sel-hover` | :hover simulado con coordenada fija (recalcula estilo) |
| `sel-media-width` | @media (max-width) aplica/no aplica al viewport |
| `sel-import` | @import encadenado (2 niveles) con ciclo cortado |

### 3.7 Fase 6 — Tablas (`table-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `table-auto-widths` | reparto de anchos auto por contenido |
| ◆ `table-collapse` | border-collapse: el borde compartido no se dobla |
| ◆ `table-spans` | colspan/rowspan: rects de las celdas restantes |
| `table-valign` | vertical-align de celda (top/middle/bottom) |
| `table-th-default` | th negrita + centrado por UA |

### 3.8 Fase 7 — Position y floats (`pos-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `pos-relative-offset` | offset visual sin alterar el flujo |
| ◆ `pos-absolute-in-relative` | containing block = ancestro posicionado |
| ◆ `pos-absolute-percent` | top/left % contra el containing block |
| ◆ `pos-fixed-viewport` | anclado al viewport, no al documento |
| ◆ `pos-float-wrap` | texto rodeando float (rects de las líneas) |
| ◆ `pos-float-clear` | clear:left/right/both empuja debajo |
| `pos-zindex-stack` | orden de pintado con z-index cruzados |

### 3.9 Fase 8 — Flexbox (`flex-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `flex-center-perfect` | centrado total (justify+align center) |
| ◆ `flex-grow-shrink` | reparto con grow/shrink y min/max por ítem |
| ◆ `flex-basis` | auto/0/px/% y flex:1 vs flex:auto vs flex:none |
| ◆ `flex-justify` | los 5 justify-content clásicos |
| ◆ `flex-align` | align-items + align-self (stretch/flex-end/center) |
| `flex-wrap` | multi-línea + align-content |
| `flex-gap-order` | gap en ambos ejes + order reordena |
| `flex-nav-app` | navbar + sidebar + main (integrador) |

### 3.10 Fase 9 — Grid (`grid-*`)

| id | qué comprueba |
|----|---------------|
| ◆ `grid-template` | fr/px/%/auto y repeat(3, 1fr) |
| ◆ `grid-placement` | grid-column/row por líneas |
| `grid-auto-flow` | auto-placement por filas |
| `grid-gap-align` | gap + align/justify-items |

### 3.11 Integradores (se corren siempre, en cada fase)

| id | qué comprueba |
|----|---------------|
| `int-blog` | post: títulos, párrafos, lista, blockquote, imagen |
| `int-cards` | grid/flex de cards con radius+shadow+imagen+ellipsis |
| `int-dashboard` | app shell: navbar fija, sidebar, main con overflow |
| `int-newsletter` | tabla old-school de email con estilos inline |

Criterio integradores: 0 diffs estructurales de rects y pixel-diff < 2%
(los integradores pueden relajar a 5% si solo hay ruido de anti-aliasing,
nunca diferencias de forma).

---

## 4. Reglas de veredicto (las que implementa `tools/cssrun.py`)

- Cada test se renderiza con nuestro motor (`--shot`) y se recorta al área
  de página; el golden es el render de **Chromium** (`--cefshot`) de la
  MISMA página, misma fuente, mismo tamaño.
- **PASS** exige las dos:
  1. **% de píxeles distintos** (luminancia > 24) por debajo del umbral del
     test — 2% por defecto, ajustable por test con
     `<!-- cssrun: threshold=3.5 -->` en el HTML para los que tengan ruido
     legítimo de anti-aliasing;
  2. **sin regiones sólidas grandes**: píxeles de diff dentro de una
     ventana 9x9 al 80% — por encima de 50, es que falta (o está
     desplazado) un elemento entero y se ve a simple vista, aunque el %
     total sea pequeño.  Esto es lo que impide que "un elemento pequeño
     ausente" se cuele bajo el porcentaje.
- **FAIL** imprime qué regla se rompió y cuánto; el render nuestro queda
  en `css-tests/out/` para compararlo a ojo con el golden.
- Un test que el oráculo renderiza distinto entre dos corridas es un bug
  del harness, no del motor: se congela (mismo binario CEF, mismas fuentes,
  sin red) y se investiga antes de seguir.
- Los tests nunca se "arreglan" relajando tolerancias: se abre variante
  `-known.html` documentando la desviación y su causa (y esa lista, corta,
  es la deuda exacta con Chromium).

## 5. Cuándo corre cada cosa

- Al desarrollar una fase: su bloque + `int-*` + todos los bloques de
  fases anteriores (regresión).
- Antes de cada commit de layout/css: `--cssdiff-all` completo.
- El corpus vive en `css-tests/<fase>/` junto al código que lo hace pasar;
  un test nuevo SIEMPRE entra en el mismo commit que la feature que lo
  hace pasar (o antes, rojo, si se desarrolla test-first).
