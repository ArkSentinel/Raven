# Lo que se implementó en este fork

Registro de lo que se añadió a Raven en la rama `localtex`, para no depender de
la memoria de nadie. Todo es código de la librería (no de la aplicación
LocalTeX), documentado en los encabezados públicos y en `docs/reference.html`.

- Rama: `localtex`
- Base: Raven 0.2.2 + `origin/main` (Plot, arreglo de #53)
- Commits: `50abc60` (base del fork) y `53d61de` (los componentes), más los
  ajustes posteriores de multi-cursor en `src/widgets/text_editor.c`

## Estado de verificación

| Comprobación | Resultado |
|---|---|
| `cmake --build` (Release, shared) | sin errores ni avisos nuevos (los dos avisos de `theme.c` y `date_picker.c` ya estaban en la base) |
| `ctest -E "render\|wayland"` | **29 ejecutados, 28 pasan, `test_web` se omite** (no hay Chromium) |
| Los 4 tests nuevos | `test_latex_editor` 173, `test_pdf_view` 106, `test_process` 100, `test_settings` 794 comprobaciones, 0 fallos |
| AddressSanitizer + UBSan (tests de editor, PDF, procesos, settings, text_editor, xml, widgets) | limpio, 0 fugas |
| `test_render` | no ejecutable aquí (necesita GPU y display; falla igual en la base, sin el fork) |
| `tools/bindgen/bindgen.py --check` | al día (11 lenguajes regenerados) |
| `tools/gen_reference_entries.py --check` | al día; es un test más del `ctest` (`reference_up_to_date`) |

## 1. Vista previa de PDF (`raven_pdf.h`, `src/widgets/pdf_view.c`)

Widget `PdfView` (`R_WIDGET_PDF_VIEW`). Raven no decodifica PDF: la aplicación
renderiza la página y le entrega el bitmap; el widget hace el resto.

- 24 funciones `Rpdf_view_*` y el enum `RPdfFit` (`None`, `Width`, `Page`).
- Páginas apiladas con hueco, sombra, fondo y borde del tema; el número de la
  página atenuado mientras falta el bitmap.
- Zoom en píxeles por punto (Ctrl+rueda, pinza táctil, o por código), `Fit`
  que sigue el tamaño del widget, scroll por barra/rueda/teclado/táctil,
  navegación por páginas.
- `Rpdf_view_set_page_needed_callback`: dice qué páginas están visibles y a
  qué escala, con deduplicación por `(página, escala)` y tolerancia del 1 %,
  para renderizar solo lo que se ve.
- SyncTeX inverso: `Rpdf_view_get_page_at` (página y punto bajo el puntero) y
  `Rpdf_view_set_page_clicked_callback`.
- SyncTeX directo: `Rpdf_view_scroll_to_position` y `Rpdf_view_set_highlight`.
- Accesibilidad: rol `R_ROLE_DOCUMENT`. Tags `PdfView`, `Pdf`, `PdfPreview`.
- Prueba: `tests/test_pdf_view.c` (106 comprobaciones).

## 2. Editor para LaTeX (`raven_text_editor.h`, `syntax.c`, `text_editor.c`)

41 funciones nuevas en el encabezado del editor, más dos lenguajes.

**Lenguajes**
- `LaTeX` y `BibTeX` integrados, con lexers propios y las banderas
  `R_LANGUAGE_TEX` / `R_LANGUAGE_BIBTEX`.
- `RLanguageDesc::word_characters`: deja que `\` (TeX) o `@` (BibTeX) sean
  parte de una palabra, lo que hace que completar `\sec` proponga `\section`
  y que las palabras del texto se cuenten como un editor de LaTeX las cuenta.
  (El emparejamiento de `\begin`/`\end` al plegar lo hace el plegador, que lee
  el nombre del entorno; no depende de esta bandera.)
- Comandos con argumento con nombre (`\documentclass{article}`,
  `\ref{...}`) colorean el argumento como tag; `$...$` como texto;
  `\begin{verbatim}` como texto literal; `%` como comentario (respetando
  `\%`).

**Al escribir**
- `\begin{itemize}` auto-completa `\end{itemize}` en la línea de abajo, con
  el cursor indentado dentro.
- Enter tras `\begin{...}` indenta la línea siguiente.
- Ctrl+/ comenta con `%`.

**Búsqueda y reemplazo** (9 funciones)
- Texto plano o expresión regular (`R_SEARCH_REGEX`), con
  `R_SEARCH_IGNORE_CASE` y `R_SEARCH_WHOLE_WORD` (las tres valen también con
  una expresión regular).
- Coincidencias calculadas una vez por (texto, patrón) y resaltadas.
- `Rtext_editor_find_next` (con vuelta), `replace`, `replace_all` como un
  solo paso del historial, y `Rtext_editor_expand_replacement` con `$1`,
  `${1}`, `${1:texto}`, `$&`, `$$`, `\n`, `\t`.
- Motor propio `src/widgets/regex.c`: alternancia, grupos, clases, `\d\w\s`,
  `\b`, anclas, `{n,m}`, perezosos. Presupuesto de pasos: un patrón
  patológico dice "sin coincidencias" en vez de colgar la ventana.

**Plegado** (9 funciones)
- Bloques: llaves/corchetes/paréntesis, `\begin`/`\end`, tags, títulos
  Markdown.
- Flecha en el margen (con números de línea), `...` al plegar, la última
  línea del bloque queda visible.
- Ctrl+Shift+[ pliega, Ctrl+Shift+] despliega; `toggle`, `fold_line`,
  `unfold_line`, `unfold_all`, `is_folded`, `get_folded_count`,
  `get_fold_start`, `set/get_folding`.

**Snippets** (10 funciones)
- `Rsnippet` con `name`, `description`, `prefix`, `body`.
- En el cuerpo: `$1`..`$9`, `${1:texto}`, `$0`, `$$`, `\n`, `\t` y `$NAME`
  (una variable que da la aplicación con `Rsnippet_set_variable`: fecha,
  autor...). Un `$NAME` sin valor se queda como está escrito.
- Tab y Shift+Tab recorren las posiciones, Escape sale.
- `Rtext_editor_snippet_at_caret` dice qué snippet expandiría la palabra que
  se escribe; `insert_snippet`, `set_snippet_completion`, `is_in_snippet`.

**Multi-cursor** (7 funciones) — éste se completó al final
- `set/get_multi_cursor`, `add_caret`, `clear_carets`, `get_caret_count`,
  `get_caret`, `select_next_occurrence`.
- Ctrl+clic añade un cursor; Alt+arrastrar selecciona una columna (un cursor
  por línea); Ctrl+D añade el siguiente trozo de texto igual al seleccionado.
  Se dibujan todos con su selección.
- Escribir, Backspace y Delete actúan en todos a la vez y como un solo paso
  del historial (una sola pulsación de Ctrl+Z los deshace todos). Internamente
  trabaja por desplazamientos absolutos de byte, edita de derecha a izquierda
  y arrastra las posiciones; los cursores que quedan en el mismo sitio se
  funden en uno.
- Prueba real: escribir en dos cursores, borrar en dos, comprobar que no se
  duplica ni se pierde texto, y que un solo Ctrl+Z lo devuelve todo.

**Lectura para la barra de estado y los marcadores** (6 funciones)
- `Rtext_editor_get_marker` (línea, columnas, tipo, mensaje),
  `clear_markers_of_line`.
- `get_word_count` (ignora los comentarios del lenguaje y, en TeX, también
  los comandos), `get_character_count` (code points, no bytes),
  `get_non_empty_line_count`, `get_current_line`.

## 3. Procesos y registros (`raven_process.h`, `raven_latex.h`)

**Procesos** (9 funciones, `src/core/process.c`)
- `Rprocess_start`: `fork`/`execvp` en POSIX y `CreateProcessW` en Windows,
  un hilo lector por proceso, entrega en el hilo de UI vía `Rtask_post` (los
  callbacks pueden tocar widgets). Sin shell.
- `cancel`, `wait`, `is_running`, `get_exit_code`, `get_state`.
- `Rprocess_run`: variante bloqueante para trabajo de preparación, salida
  acotada y cortada en frontera de carácter UTF-8.
- `Rprocess_find` / `is_available`: PATH y carpetas habituales de TeX Live y
  MiKTeX por plataforma.
- `Rshutdown` cancela y libera lo que siga vivo.

**Registros de LaTeX** (5 funciones, `src/core/latex_log.c`)
- `Rlatex_log_parse` / `parse_file` devuelven un **handle** que se lee con
  `Rlatex_log_get_count` y `Rlatex_log_get_message` (el resto de Raven
  devuelve handles; así lo entienden los 11 generadores de bindings).
- Detecta: errores `! ...` con `l.N` y archivo, `Package X Error:`,
  avisos de `LaTeX`/`Package`/`Class` con `on input line N`, `Overfull`/
  `Underfull`, `Emergency stop`, `Fatal error`, `No pages of output`.
  Sigue la pila de archivos abiertos con paréntesis.

**Compilación** (9 funciones, `src/core/latex_build.c`)
- `Rlatex_build_start(&desc, on_output, on_finished, user_data)`: lanza
  latexmk o un motor, transmite la salida, y al terminar parsea el log y
  guarda los mensajes. Nunca bloquea.
- `Rlatex_build_get_messages` (handle), `get_pdf_path` (prefiere lo que diga
  `Output written on ...`), `get_log_path`, `succeeded`, `cancel`,
  `destroy`, `is_running`.
- `Rlatex_find_tool`.
- Prueba: `tests/test_process.c` (100 comprobaciones), incluido un build de
  mentira con `command_override` cuyo log se parsea.

## 4. Configuración (`raven_settings.h`) y lo que se apoya en ella

**Almacén** (23 funciones, `src/core/settings.c`)
- JSON con claves punteadas; bool, int, double, texto; documento completo
  (`get_json`/`set_json`), objetos anidados aplanados, arrays, comentarios
  `//` y coma final tolerados.
- Guardado atómico; lector robusto (profundidad máxima, sin búferes de
  tamaño fijo, `\uXXXX` con pares sustitutos).
- `Rsettings_open` crea la carpeta habitual de la plataforma:
  `~/.config/<app>`, `~/Library/Application Support/<app>`,
  `%APPDATA%\<app>`. El archivo lo escribe el primer `Rsettings_save`.
- Libera todo en `Rshutdown`.

**Keybindings configurables** (5 funciones)
- `Rwindow_set_keybindings` desde texto (`"Ctrl+S save | F5 build"`),
  `Rkey_from_name`, `Rkey_to_name`, `Rwindow_get_shortcut_count`,
  `Rwindow_get_shortcut`. Permite una ventana de preferencias sin tabla
  compilada. (Las secuencias de varios acordes se parsean, pero solo se
  enlaza el primero.)

**Tema por proyecto** (3 funciones)
- `Rtheme_remember_for`, `Rtheme_get_remembered`, `Rtheme_apply_remembered`,
  con la ruta del proyecto normalizada a absoluta como clave.

**Prueba**: `tests/test_settings.c` (794 comprobaciones), incluido un fuzz de
200 000 documentos sobre `Rsettings_set_json` (bytes al azar, y piezas válidas
en órdenes que no lo son) bajo ASan, que además relee lo aceptado para
comprobar que el lector y el escritor coinciden.

## 5. Issue #20: la interfaz deja de ir por detrás del puntero

- Listas, tablas, árboles y explorador de archivos: la barra de selección ya
  no persigue al puntero. Un clic la pone donde está el clic, al instante; solo
  el teclado la desliza. `Rset_selection_animation` / `Rget_selection_animation`
  permiten recuperar el deslizamiento (por defecto, desactivado).
- Redimensionar una ventana nunca se anima: el área de cliente sigue al
  compositor exactamente. En Wayland, el surface se confirma en cuanto se
  conoce el tamaño nuevo y se pide un frame, en vez de esperar al siguiente
  búfer por otro motivo.
- El cursor de texto sigue deslizándose (es lo que se espera de un cursor).

## 6. Documentación y herramientas

- `docs/LOCALTEX.md`: guía de todo lo anterior, con ejemplos.
- `docs/XML.md`: `PdfView` (sección nueva) y los atributos nuevos del
  `TextEditor` (`Folding`, `Snippets`, `MultiCursor`, `LaTeX`/`BibTeX`).
- `docs/reference.html`: todas las funciones nuevas, con la firma que da la
  cabecera. `tools/gen_reference_entries.py` las añade y corrige las que se
  quedaron atrás (`--check`); es el test `reference_up_to_date` del `ctest`.
- `README.md`: enlace a `docs/LOCALTEX.md`.
- `tools/vscode-raven/src/schema.json`: regenerado (PdfView, atributos y
  lenguajes nuevos).
- `bindings/`: regenerados para los 11 lenguajes.
- `MISSING_COMPONENTS.md`: el documento de partida, se conserva.

## Lo que queda fuera de la librería

En `docs/LOCALTEX.md` está explicado; resumen: unir compilación + log +
marcadores + render es trabajo de la app; SyncTeX se invoca desde la app (la
librería tiene los dos extremos); el corrector es hunspell por
`Rprocess_start`; `.bib`/Zotero, plantillas, CRDT/WebSocket, terminal y Ollama
son funciones de aplicación; y el empaquetado, firmado, notarización y App
Store son trabajo de distribución.
