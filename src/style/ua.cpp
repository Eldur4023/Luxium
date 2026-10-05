// The browser's own stylesheet: Blink's html.css, trimmed to what Luxium
// renders.  It goes through the same parser and cascade as page CSS, so a
// page rule `h1 { color: X }` beats it by specificity/order, no special case.

namespace luxium::style {

extern const char* const kUAStylesheet;
const char* const kUAStylesheet = R"css(
html { display: block }
head, link, meta, script, style, title, base, template, noscript, area, datalist, param, source, track { display: none }
body { display: block; margin: 8px }
address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing,
main, p, plaintext, pre, search, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section,
dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup, option { display: block }
li { display: list-item }
table { display: table }
caption { display: table-caption }
colgroup { display: table-column-group }
col { display: table-column }
thead { display: table-header-group }
tbody { display: table-row-group }
tfoot { display: table-footer-group }
tr { display: table-row }
td, th { display: table-cell }
button, input, select, textarea { display: inline-block }
br, wbr { display: inline }

h1 { font-size: 2em; font-weight: bold; margin: 0.67em 0 }
h2 { font-size: 1.5em; font-weight: bold; margin: 0.83em 0 }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0 }
h4 { font-size: 1em; font-weight: bold; margin: 1.33em 0 }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0 }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0 }
p, dl, menu { margin: 1em 0 }
blockquote { margin: 1em 40px }
figure { margin: 1em 40px }
ul, ol, dir, menu { margin: 1em 0; padding-left: 40px }
ul { list-style-type: disc }
ol { list-style-type: decimal }
ul ul, ol ul { list-style-type: circle }
ul ul ul, ol ul ul, ul ol ul, ol ol ul { list-style-type: square }
ul ol, ol ol { list-style-type: decimal }
dd { margin-left: 40px }
ol ul, ul ol, ul ul, ol ol { margin-top: 0; margin-bottom: 0 }
fieldset { margin: 0 2px; padding: 0.35em 0.75em 0.625em; border: 2px groove #c0c0c0 }
pre, listing, plaintext, xmp { font-family: monospace; font-size: 13px; white-space: pre; margin: 1em 0 }
code, kbd, samp, tt { font-family: monospace; font-size: 13px }
b, strong { font-weight: bold }
i, em, cite, var, address, dfn { font-style: italic }
u, ins { text-decoration: underline }
s, strike, del { text-decoration: line-through }
small { font-size: smaller }
big { font-size: larger }
sub { vertical-align: sub; font-size: smaller }
sup { vertical-align: super; font-size: smaller }
mark { background-color: yellow; color: black }
a:link { color: #0000ee; text-decoration: underline; cursor: pointer }
a:visited { color: #551a8b }
hr { margin: 0.5em auto; border: 1px inset; color: gray; overflow: hidden }
center { text-align: center }

table { box-sizing: border-box; border-collapse: separate; border-spacing: 2px }
thead, tbody, tfoot, tr { vertical-align: middle }
td, th { padding: 1px; vertical-align: inherit }
th { font-weight: bold; text-align: center }
caption { text-align: center }

button { font-family: Arial; font-size: 13.333px; color: black; background-color: #efefef; padding: 1px 6px;
         border: 2px outset #767676; text-align: center; border-radius: 3px }
input, textarea, select { font-family: Arial; font-size: 13.333px }
img { overflow: clip }
[hidden] { display: none }
)css";

} // namespace luxium::style
