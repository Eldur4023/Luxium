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
         border: 2px outset #767676; text-align: center }
button, input, textarea, select { line-height: normal }
input, textarea, select { font-family: Arial; font-size: 13.333px; color: black; margin: 0; text-align: start }
input { padding: 1px 2px; border: 2px inset #767676; background-color: white; overflow: clip }
input[type=checkbox], input[type=radio] { box-sizing: border-box; padding: 0; border: none; background-color: transparent;
                                          margin: 3px 3px 3px 4px; overflow: visible }
input[type=radio] { margin: 3px 3px 0 5px }
input[type=range] { padding: 0; border: none; margin: 2px; overflow: visible; color: rgb(157, 150, 142) }
input[type=search] { box-sizing: border-box }
input[type=file] { padding: 0; border: none; background-color: transparent; white-space: pre; align-items: baseline }
input[type=submit], input[type=button], input[type=reset] { box-sizing: border-box; text-align: center; white-space: pre;
         padding: 1px 6px; border: 2px outset #767676; background-color: #efefef; overflow: visible }
textarea { font-family: monospace; padding: 2px; border: 1px solid #767676; background-color: white; white-space: pre-wrap; overflow: auto }
select { box-sizing: border-box; border: 1px solid #767676; background-color: #efefef; white-space: pre; align-items: center }
input:disabled, textarea:disabled, select:disabled { color: #545454; background-color: rgba(239, 239, 239, 0.3);
                                                      border-color: rgba(118, 118, 118, 0.3) }
button:disabled { color: rgba(16, 16, 16, 0.3); background-color: rgba(239, 239, 239, 0.3); border-color: rgba(118, 118, 118, 0.3) }
input[type=hidden] { display: none }
html[data-luxium-scheme=dark] { color: white; background-color: #121212 }
html[data-luxium-scheme=dark] input:not([type=checkbox]):not([type=radio]):not([type=range]):not([type=file]):not([type=submit]):not([type=button]):not([type=reset]), html[data-luxium-scheme=dark] textarea { color: white; background-color: #3b3b3b; border-color: #858585 }
html[data-luxium-scheme=dark] select, html[data-luxium-scheme=dark] button, html[data-luxium-scheme=dark] input[type=submit] {
    color: white; background-color: #6b6b6b; border-color: #858585 }
:focus-visible { outline: auto 1px -webkit-focus-ring-color }
input:focus-visible, textarea:focus-visible, select:focus-visible { outline-offset: 0 }
img { overflow: clip }
[hidden] { display: none }
dialog:not([open]) { display: none }
dialog { position: absolute; inset-inline: 0; width: fit-content; height: fit-content; margin: auto; border: solid; padding: 1em;
         background: white; color: black }
)css";

} // namespace luxium::style
