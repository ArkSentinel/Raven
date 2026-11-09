# Registro de cambios

Formato inspirado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).
Este fork (`localtex`) parte de Raven 0.2.2 (base `32722ad`). Lo que sigue es lo que
cambia respecto a la base; la API completa está en `docs/reference.html`.

## Sin publicar

### Añadido
- Editor de texto: `Rtext_editor_toggle_comment`, `Rtext_editor_begin_edit` /
  `Rtext_editor_end_edit` (agrupar ediciones en un solo paso de undo),
  `Rtext_editor_is_modified` / `Rtext_editor_set_modified`,
  `Rtext_editor_reload_text` (recargar sin perder caret ni scroll),
  `Rtext_editor_add_marker_from` y `Rtext_editor_clear_markers_of_source` (marcadores
  con origen, para que dos fuentes no se pisen).
- Pestañas: `Rtab_view_move_tab`.
- Texturas: `Rtexture_is_placeholder`, que indica las que quedaron vacías tras perder
  el dispositivo.
- Ventanas: `Rwindow_has_surface`.
- Visor PDF: `Rpdf_view_set_page_changed_callback`.
- Procesos: `Rprocess_find_copy`, copia de la ruta que no cambia con la siguiente búsqueda.
- Pestañas: `Rtab_view_set_reordered_callback`, avisa al mover una pestaña.
- Ventanas: `Rwindow_get_surface_error`, texto del último fallo de superficie.
- Archivos: `Rfile_copy`, `Rfile_move` (copia si no puede renombrar) y `Rfile_mkdir_p`.
- Visor PDF: `Rpdf_view_add_link`, `Rpdf_view_clear_links` y
  `Rpdf_view_set_link_clicked_callback`: la aplicación da los enlaces de cada página
  (Raven no lee el PDF) y recibe el clic dentro de uno.
- Ajustes: `Rsettings_remove_prefix`, para borrar una lista entera (p. ej. `project.recent`).
- Proceso: `Rprocess_destroy` (ya en la base del fork desde `53d61de`).

### Corregido
- Handles: la generación de 8 bits daba la vuelta y un `Rid` viejo volvía a ser
  válido. Un índice cuya generación da la vuelta se retira en lugar de reutilizarse.
- Superficie de Vulkan: `used` se marca solo tras el último paso que puede fallar.
- Designer: un cambio de archivo se detecta también por el tamaño, no solo por la fecha
  (dos escrituras seguidas podían tener la misma fecha y la recarga no ocurría).
- Bindings: regenerados con `tools/bindgen/bindgen.py`; el binding C++ se compila con
  C++17 (el header necesita C++14 como mínimo, documentado en `bindings/README.md`).

### Cambiado
- `docs/reference.html` regenerado con `tools/gen_reference_entries.py`.

## Fork `localtex`

### 115c55e — bindgen/cpp: una fábrica mantiene un callable por llamada
### 54c7466 — localtex: varios cursores que escriben, un registro del trabajo
### 53d61de — localtex: visor PDF, editor LaTeX, ejecutor de procesos y analizador de log, ajustes, sin lag bajo el puntero
### 50abc60 — localtex: base del fork en Raven 0.2.2 y upstream main (Plot, #53)
