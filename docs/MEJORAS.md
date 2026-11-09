# Raven: lo que falta mejorar

Una revisión del fork `localtex` de Raven (0.2.2) hecha desde LocalTeX, que es
su primer consumidor real. No es una lista de deseos: cada punto dice **qué
falla hoy**, **dónde**, **por qué importa a una aplicación** y **qué haría
falta**. Lo que se pudo comprobar se comprobó; lo que es una impresión de
lectura se marca como tal.

Fecha de la revisión: nov 2026. Base: `../Raven`, Linux, GCC 14, Vulkan con
lavapipe y NVIDIA.

---

## 0. Resumen

| Prioridad | Tema | Estado verificado |
|---|---|---|
| **P0** | `test_render` hace segfault dentro del ICD de NVIDIA cuando hay varios ICD (ver §1.1: reproducción mínima) | **abierto**: causa no hallada en Raven; falta decidir cómo reportarlo |
| **P0** | `bindings_up_to_date` falla: los bindings del repo no coinciden con las cabeceras | **hecho**: regenerados con `bindgen.py`, `--check` pasa |
| **P0** | `binding_cpp` no compila: el header del binding C++ no es C++11 | **hecho**: `CXX_STANDARD 17`; el mínimo real es C++14 (`bindings/README.md` corregido) |
| **P0** | El CI de `main` está en rojo por los tres anteriores | pendiente de comprobar en CI; en local solo queda `test_render` |
| **P1** | La generación del handle es de 8 bits: un `Rid` viejo vuelve a ser válido tras 256 reúsos del índice | **hecho**: un índice cuya generación da la vuelta se retira; test en `test_widgets` (falla sin el cambio) |
| **P1** | `Rprocess_*` no tiene `destroy` público (LocalTeX tuvo que añadirlo) | **hecho**: ya exportado en `raven_process.h` |
| **P1** | Un error de superficie solo va al log: la app no puede consultar qué ventana quedó sin dibujar | **parcial**: `Rwindow_has_surface` hecho; el callback por ventana y el estado de backend quedan pendientes |
| **P2** | Faltan operaciones de editor que todo editor de texto necesita (comentar, mover pestañas, agrupar el undo) | **hecho** en lo principal: comentar, begin/end, modified, mover pestañas y recarga (§3.1–3.5); quedan el callback `reordered` y el ancho de línea (§5) |
| **P2** | El PDF view no expone el texto de la página (búsqueda, selección, copiar) | verificado por grep |
| **P2** | Tras recuperar el dispositivo, la app no sabe qué texturas quedaron vacías | **hecho**: `Rtexture_is_placeholder`; test `test_lost_placeholders` |
| **P3** | Documentación de API dispersa entre `reference.html` y las cabeceras; sin changelog | por lectura |

Resultado de `ctest` en el fork al empezar (32 pruebas): **29 pasan, 3 fallan**.
Al terminar esta ronda (50 pruebas): **49 pasan, 1 falla (`test_render`), 1 omitida (`test_web`)**.

```
26 - test_render (SEGFAULT)
28 - bindings_up_to_date (Failed)
29 - binding_cpp (Not Run)     <- el binario no existe porque no compila
```

---

## 1. P0: la suite de pruebas no está verde

### 1.1 `test_render` hace segfault

> **Corrección (ronda de revisión posterior).** La causa que se escribió abajo
> no se sostiene. Comprobado con gdb: en el crash `g_vk.physical_device` y
> `g_vk.instance` son válidos, y el salto a dirección 0 ocurre **dentro de
> `libnvidia-glcore`**, llamado desde el trampolín del loader de
> `GetPhysicalDeviceSurfaceSupportKHR`. Con `RAVEN_VK_DEVICE=0` o `=2` la prueba
> pasa; con `=1`, `=3`, `=4`, `=5` o `=6` crashea. Con `VK_ICD_FILENAMES` apuntando
> solo a NVIDIA la prueba no crashea, pero da 2 fallos de píxel
> (`test_render.c:720-721`). Con solo LVP o solo Intel la prueba **pasa** (290
> comprobaciones), no se salta: la afirmación de más abajo sobre el skip era
> incorrecta. Un handle de superficie nulo **no** se ha observado. Hipótesis
> pendiente: un dispositivo de un ICD combinado con el dispositivo o la
> instancia de otro en el loader multi-ICD. Cambio hecho: `used` se marca
> después del último paso que puede fallar (`vk_surface.c`), pero no arregla el
> crash.
>
> **Reproducción mínima (ronda posterior).** Con `VK_ICD_FILENAMES` apuntando a
> `nvidia_icd.json` más **cualquiera** de `intel_icd.json`, `lvp_icd.json`,
> `nouveau_icd.json`, `virtio_icd.json`, `radeon_icd.json` o `intel_hasvk_icd.json`,
> `test_render` crashea. Con `nvidia_icd.json` solo, o con `gfxstream_vk_icd.json`
> (que no tiene dispositivos y el loader lo quita), no crashea. Dispositivos
> enumerados: 0 Intel HD 630, 1 GTX 1050 (NVIDIA), 2 llvmpipe. El crash ocurre en
> la llamada a `GetPhysicalDeviceSurfaceSupportKHR` con la GTX 1050, con el handle
> de superficie válido (en depuración: `surface->surface` no es nulo) y el
> physical device válido; el salto va a la dirección `0x2` dentro de
> `libnvidia-glcore`. No depende de la capa MESA device-select (probado con
> `NODEVICE_SELECT=1` y con `VK_LOADER_LAYERS_DISABLE=all`), ni de Wayland (también
> con X11). Versiones: libvulkan 1.4.309, NVIDIA 550.163.01, Mesa 25.0.7.
> **Qué falta:** decidir si se reporta al proveedor (NVIDIA/loader) con esta
> reproducción, o si Raven evita combinar ICD en la prueba. No se ha hallado un
> fallo en el código de Raven que lo cause.


**Qué pasa.** Con un driver Vulkan disponible, `test_render` muere con
`SIGSEGV` en el hilo `raven-render`, dentro de `libnvidia-glcore.so`, llamado
desde `rvn_vk_surface_create`.

```
Thread 4 "raven-render" received signal SIGSEGV
#0  libnvidia-glcore.so
#1  rvn_vk_surface_create (window=268435456, native=..., width=..., height=64)
    at src/render/vulkan/vk_surface.c:432
#2  rvn_render_thread at src/render/renderer.c:171
```

**Causa.** En `vk_surface.c`:

```c
memset(surface, 0, sizeof(*surface));
if (!rvn_vk_native_surface_create(native, &surface->surface)) return R_FALSE;  // (a)
surface->used = R_TRUE;
...
rvn_vk.GetPhysicalDeviceSurfaceSupportKHR(g_vk.physical_device, g_vk.queue_family,
                                          surface->surface, &supported);       // (b)
```

Si (a) falla, `surface->surface` es `VK_NULL_HANDLE` y se sale sin limpiar
`surface->used` (que ya era 0 por el `memset`, bien), pero el problema real es
que **el error de (a) no se propaga hasta la aplicación**: `rvn_rcmd_execute`
devuelve el resultado a `rvn_rcmd_submit`, pero el camino de "crear superficie"
lo ignora cuando no se pidió espera. Y en el caso que nos ocupa, (b) se ejecuta
con un handle nulo en algunos drivers, que en vez de devolver
`VK_ERROR_INITIALIZATION_FAILED` revientan.

Además, el caso `native->system == R_WINDOW_SYSTEM_NONE` solo funciona si
`VK_EXT_headless_surface` está disponible **y** `CreateHeadlessSurfaceEXT` fue
cargado; si no, cae al `result` inicial (`VK_ERROR_EXTENSION_NOT_PRESENT`) y se
trata como error, pero el test espera que la ausencia de driver sea un *skip*
(código 77), no un fallo.

**Por qué importa.** Es la única prueba que valida píxeles reales. Un
segfault en ella significa que nadie puede confiar en el backend Vulkan en CI,
y un usuario con un driver que rechace la superficie ve una ventana negra sin
mensaje.

**Qué haría falta.**
1. Comprobar `surface->surface != VK_NULL_HANDLE` antes de (b), y liberar con
   `rvn_vk_surface_release` si algo falla a mitad.
2. Propagar el fallo de `CreateSurface` a `Rwindow_*` para que la aplicación
   pueda mostrar "no se pudo crear la superficie" en vez de quedarse muda.
3. En `test_render`, distinguir "no hay driver utilizable" (skip 77) de "el
   driver existe y falla" (fallo con mensaje), que hoy se mezclan.

**Reproducir.**

```sh
cmake -S ../Raven -B /tmp/raven-build -G Ninja -DRAVEN_BUILD_TESTS=ON
cmake --build /tmp/raven-build
gdb -batch -ex run -ex "bt 20" --args /tmp/raven-build/bin/test_render
```

(Con `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` el test **pasa**
(290 comprobaciones), no se salta. Ver la corrección al principio de §1.1.)

### 1.2 `bindings_up_to_date` falla

**Qué pasa.**

```
bindings/ is out of date (run tools/bindgen/bindgen.py): bun/src/raven.ts,
c3/..., cpp/raven.hpp, csharp/Raven.cs, go/..., java/..., luajit/..., odin/...,
python/raven.py, rust/src/sys.rs, zig/raven.zig
```

Los doce bindings del árbol no coinciden con las cabeceras. Parte es esperable
en un fork con cambios locales —LocalTeX añadió `Rprocess_destroy`—, pero el
test no distingue *"el fork cambió la API y hay que regenerar"* de *"alguien
editó un binding a mano"*.

**Por qué importa.** El test existe para que los bindings no se desincronicen
en silencio. Si falla de forma crónica, deja de proteger nada: es la alarma
que todos ignoran.

**Qué haría falta.**
1. Regenerar con `tools/bindgen/bindgen.py` y commitear el resultado como un
   paso explícito del flujo de cambio de API (un `make bindings`).
2. Que `bindgen.py --check` diga **qué** cambió (función añadida, quitada,
   firma distinta), no solo qué archivos están viejos.
3. Registrar en `docs/` la regla: "toda función nueva en `include/raven/`
   regenera los bindings en el mismo commit".

### 1.3 `binding_cpp` no compila

**Qué pasa.** `bindings/CMakeLists.txt` compila el ejemplo del binding C++ con
`CXX_STANDARD 11`, pero `raven.hpp` usa inicialización con llaves de un
aggregate con inicializadores por defecto (`Kept{ callable, ... }` en
`detail::keep_new`), que es C++14. El compilador falla y el test queda como
"Not Run" porque el binario nunca se produce.

```
error: no matching function for call to 'raven::detail::Kept::Kept(<brace-enclosed initializer list>)'
```

**Por qué importa.** El binding C++ es el que usa LocalTeX. Que su propio
ejemplo no compile con el estándar declarado significa que cualquiera que
integre Raven con `CMAKE_CXX_STANDARD 11` (el mínimo que promete el proyecto)
se topa con un error que parece suyo.

**Qué haría falta.** Subir el estándar declarado del binding a 17 (que es el
que usa Raven internamente y el que usan los consumidores reales) **o** quitar
la inicialización con llaves que requiere C++14. La primera opción es más
honesta: si el header no compila en C++11, no se debe declarar C++11.

---

## 2. P1: cosas que muerden al integrar

### 2.1 La generación del handle es de 8 bits

> **Estado.** Arreglado sin cambiar el ABI: en `src/core/handle.c`
> (`rvn_pool_remove`), un índice cuya generación da la vuelta a 0 se retira en
> lugar de volver a la lista libre. Test: `test_stale_handle_stays_invalid` en
> `tests/test_widgets.c`, que falla sin el cambio. Coste: cada índice se usa como
> mucho 255 veces antes de retirarse; los 2^20 índices siguen disponibles.
> La propuesta de abajo de un índice de 12 bits **no** es válida: el layout real
> es tipo 4 bits, generación 8, índice 20 (`src/core/handle.h`), así que 12 bits
> de índice bajarían el máximo de 1M a 4096.


Corrección de una primera impresión: el pool **sí** recicla índices. En
`src/core/handle.c`, `rvn_pool_remove` empuja el índice a una lista libre y
`rvn_pool_add` lo reutiliza. Eso está bien hecho, y hay un techo de índices con
error (`RVN_HANDLE_MAX_INDEX`) en vez de desbordar.

Lo que falla es la **generación**:

```c
RVN_INLINE uint32_t rvn_handle_generation(Rid id) { return (id >> RVN_HANDLE_INDEX_BITS) & 0xFFu; }
```

Ocho bits. La generación se incrementa en cada `rvn_pool_remove`
(`pool->generations[index]++`), así que un índice que se crea y destruye 256
veces vuelve a la generación con la que empezó. Un `Rid` viejo que la
aplicación guardó por error (un documento cerrado, una textura liberada) pasa a
ser **válido otra vez** y apunta a un objeto distinto.

**Por qué importa.** El caso típico es exactamente el de un editor: un panel de
pestañas donde las pestañas se abren y cierran todo el día. Un callback que se
quedó con el `Rid` de un editor cerrado no recibe `R_INVALID_ID` (que es lo que
la comprobación de generación promete) sino que escribe en el editor que ahora
ocupa ese índice. Es un use-after-free que se manifiesta como "se guardó el
texto en el archivo equivocado" o "el cursor saltó solo", horas después.

**Reproducción.** Confirmado con un programa de 30 líneas contra la biblioteca
de este árbol: se crea un widget, se guarda su `Rid`, se destruye, y se crean y
destruyen 300 más. El handle obsoleto **vuelve a ser válido**:

```
$ ./gen
stale handle became valid again: YES (bug)
```

(El sondeo imprime `Rwidget_is_valid(stale) = 0` solo porque en ese instante el
índice estaba libre; si se consulta mientras otro objeto ocupa el índice, la
respuesta es `1` y el objeto es el nuevo.)

**Qué haría falta.**
1. Generación de 16 bits (el `Rid` es de 32: 4 de tipo, 12 de índice, 16 de
   generación dan 4096 objetos simultáneos por tipo, que es de sobra para
   widgets y suficiente para texturas si se separan los tipos).
2. O bien un `Rid` de 64 bits, que además quitaría el techo de 4096 objetos.
3. Un test de reutilización como el de arriba, en `tests/`.

Nota: LocalTeX no ha sufrido este bug en la práctica (las sesiones de prueba
son cortas), pero el patrón de pestañas que se abren y cierran lo alcanza con
uso real.

### 2.2 `Rprocess_destroy` no existía (y LocalTeX lo tuvo que añadir)

**Qué pasa.** La documentación dice, en `raven_process.h`: *"The processes a
program starts belong to the library. Rshutdown asks every process still
running to stop, waits for it and frees it; there is nothing to destroy."*
Eso es cierto para un programa que lanza tres procesos y termina, no para uno
que lanza uno cada pocos segundos.

`src/core/process.c` ya tenía `rvn_process_destroy_id`, pero no se exportaba.
LocalTeX añadió `Rprocess_destroy` (cabecera, implementación y binding C++) en
esta revisión. Es una señal de diseño: **el ciclo de vida debería ser explícito
en la API**, no "todo vive hasta el shutdown".

**Qué haría falta.**
1. Adoptar el `Rprocess_destroy` añadido (o equivalente) como contrato público.
2. Aplicar el mismo criterio a `Rlatex_build_destroy` (ya existe) y a cualquier
   otro pool con recursos caros (texturas, shaders).
3. Documentar en cada familia *quién* posee el handle y *cuándo* se libera.

### 2.3 Un error de superficie se avisa por el log, no por el estado

**Qué pasa.** La creación de la superficie **sí** se espera
(`rvn_renderer_create_surface` llama a `rvn_rcmd_submit(cmd, R_TRUE)`) y su
fallo **sí** se reporta:

```c
if (!rvn_renderer_create_surface(...)) {
    rvn_error(R_ERROR_BACKEND, R_SEVERITY_ERROR, "Rwindow_create: cannot create the rendering surface");
    ...
}
```

Así que la primera impresión ("el error no llega") era falsa. El problema real
es más pequeño pero sigue existiendo: el error va a `Rerror_set_callback` /
`stderr` y **no queda en ningún estado consultable**. Una aplicación que quiere
mostrar "tu driver no puede dibujar esta ventana" en la propia ventana (o
reintentar con otro backend) no tiene a qué preguntar; solo puede leer la
cadena de error que le llegó por el callback, que no está asociada a ninguna
ventana en concreto.

Además, en el camino de fallo de `rvn_vk_surface_create` queda un estado
inconsistente: `surface->used = R_TRUE` se pone **antes** de las llamadas que
pueden fallar y devolver `R_FALSE` sin liberar ni volver a poner `used` a
falso. La siguiente creación en ese índice llama a `rvn_vk_surface_release`
sobre una superficie medio construida, lo que funciona por suerte (todo está a
cero) pero no es intencional.

**Qué haría falta.**
1. Poner `surface->used = R_TRUE` **después** de la última comprobación que
   puede fallar, o liberar en cada salida temprana.
2. Un estado por ventana (`Rwindow_get_backend_state`) y/o un callback
   `Rwindow_set_backend_error_callback` para que la app sepa qué ventana quedó
   sin dibujar.

   > **Estado.** Parcial: `Rwindow_has_surface(window)` devuelve si la ventana
   > tiene superficie (R_FALSE tras un fallo al recrearla tras perder el
   > dispositivo). Y `Rwindow_get_surface_error`, con el texto del último fallo (NULL si la superficie está).
   > Test: `test_device_lost` en `tests/test_widgets.c`.
3. Distinguir en el mensaje *"no hay driver"* (recuperable: instalar uno) de
   *"el driver rechazó esta ventana"* (p. ej. sesión remota sin DRI).

### 2.4 La recuperación tras perder el dispositivo está bien; falta saber qué se perdió

**Qué pasa.** Esta sección empezó siendo una queja y la lectura la desmintió: la
recuperación existe y es seria (`src/render/device.c`). Tras `VK_ERROR_DEVICE_LOST`,
Raven reinicia el backend, rehace las superficies de todas las ventanas, **restaura
cada textura con el mismo handle** (desde su copia de píxeles, su archivo o
transparente del mismo tamaño), reconstruye el atlas de glifos y los shaders, y
avisa a la aplicación con `Rset_device_reset_callback`. Está muy bien.

Lo que falta es el caso en que una textura **no** se puede restaurar: las que
venían de píxeles crudos y eran demasiado grandes para guardar una copia
vuelven transparentes. La aplicación no sabe *cuáles*, así que su único recurso
es volver a subir todas (caro: son las páginas del PDF) o dejar huecos.

**Qué haría falta.** Pasar al callback de reset la lista (o un contador y un
consulta por `Rid`) de las texturas que quedaron vacías, o un
`Rtexture_is_placeholder(texture)`. Con eso una aplicación re-sube solo lo
perdido.

> **Estado.** Hecho con la segunda opción: `Rtexture_is_placeholder(texture)`.
> La restauración en `src/render/texture.c` marca la textura cuando no tenía
> copia de píxeles, y cualquier subida (`Rtexture_update` o carga) lo borra.
> Test: `test_lost_placeholders` en `tests/test_widgets.c`.

---

## 3. P2: faltan operaciones que un editor de texto espera

Todas están comprobadas con `grep` sobre `include/raven/`: no existen.

> **Estado.** Hechos: `Rtext_editor_toggle_comment` (§3.1),
> `Rtext_editor_begin_edit` / `Rtext_editor_end_edit` (§3.3),
> `Rtext_editor_is_modified` / `Rtext_editor_set_modified` (§3.4). Pendiente:
> §3.5: `Rtext_editor_reload_text` conserva caret (acotado al nuevo texto) y
> scroll, y cuenta el texto como guardado. Pruebas en `tests/test_text_editor.c`
> (`test_application_edits`, `test_reload_text`). Limitación documentada:
> deshacer hasta el texto guardado sigue contando como modificado.

### 3.1 Comentar y descomentar líneas desde la API

`Ctrl+/` funciona **porque el editor lo maneja como tecla**
(`rvn_ed_toggle_comment`), pero no hay `Rtext_editor_toggle_comment`. Una
aplicación que quiere un ítem de menú "Comentar" (LocalTeX lo tiene, en
*Edit*) no puede llamar a la función: tiene que reimplementar la regla del
lenguaje (indentación, `%`, descomentar si todas ya lo están) fuera del editor.
LocalTeX lo hizo en `actions.cpp` y es una duplicación inevitable mientras
falte.

**Propuesta:**

```c
RAVEN_API int Rtext_editor_toggle_comment(Rid editor);  /* R_FALSE si el lenguaje no tiene comentario */
```

### 3.2 Mover y reordenar pestañas

Existen `Rtab_view_set_active`, `find_tab`, `set_tab_title`, `show_tab`, pero
no hay forma de mover una pestaña de posición, ni de saber si el usuario
arrastró una. Un editor con muchas pestañas necesita ordenar por uso o
permitir reordenar.

> **Estado.** Hecho: `Rtab_view_move_tab(tabs, from, to)` (reordena el array de
> hijos sin desmontar la página; la pestaña activa sigue activa). Test:
> `test_move_tab` en `tests/test_widgets.c`. Callback `reordered` añadido
> (`Rtab_view_set_reordered_callback`), con from/to; no se llama si no se movió nada.

**Propuesta original:** `Rtab_view_move_tab(Rid tabs, int from, int to)` y un callback
`reordered`.

### 3.3 Agrupar ediciones en un paso de undo

`Rtext_editor_replace_all` ya agrupa internamente, pero una operación de la
aplicación que toca varias líneas (por ejemplo, comentar un rango línea a
línea) produce **N pasos de undo**. LocalTeX lo resolvió editando de abajo
hacia arriba, que evita el problema de los índices pero no el del historial:
un Ctrl+Z deshace solo una línea.

**Propuesta:** `Rtext_editor_begin_edit(Rid)` / `Rtext_editor_end_edit(Rid)`
(lo que internamente ya son `rvn_ed_group_begin`/`_end`), expuestos.

### 3.4 Saber si el documento cambió

El editor avisa con el callback de cambio, pero no hay
`Rtext_editor_is_modified`. La aplicación mantiene su propio `bool modified`
y tiene que acordarse de ponerlo a `false` tras guardar; si se olvida en un
camino (guardar como, recargar por cambio externo, deshacer hasta el estado
inicial), el título miente. Es un estado que el editor ya conoce (tiene el
historial).

**Propuesta:** `Rtext_editor_is_modified` y `Rtext_editor_set_modified`
(para el caso "leí el archivo del disco, esto es lo guardado").

### 3.5 Recargar el texto sin perder el caret ni el scroll

`Rtext_editor_set_text` reinicia el caret y el scroll. Para la detección de
cambios externos (LocalTeX relee el archivo cuando otro programa lo guardó)
hay que guardar línea/columna/scroll antes y restaurarlos después. Un
`Rtext_editor_reload_text` que preserve la posición sería más seguro, y
permitiría además conservar los marcadores que no caen en líneas cambiadas.

---

## 4. P2: el visor de PDF

El widget `R_WIDGET_PDF_VIEW` está bien: pide páginas bajo demanda, acepta
texturas, tiene zoom y resaltado. Lo que falta es lo que convierte un visor en
un lector.

### 4.1 Texto de la página

> **Decisión (ronda posterior):** no se implementa ahora. El roadmap de LocalTeX
> no pide búsqueda ni selección de texto en el PDF (SyncTeX ya funciona por caja).
> Si se pide, la opción (b) encaja con la arquitectura: LocalTeX abre PDFium con
> `dlopen`, así que el texto lo daría la aplicación. Contexto: Raven no enlaza PDFium: su cabecera
> `raven_pdf.h` dice que la aplicación renderiza las páginas (PDFium, Quartz,
> pdftoppm). Así que `FPDFText_*` no está en el árbol y no hay nada que envolver.
> Hay que elegir: (a) Raven enlaza PDFium y expone `Rpdf_view_get_text`; o (b) la
> aplicación da el texto con un callback por página (rectángulos por carácter),
> y Raven hace la búsqueda y la selección sobre esos datos. La opción (b) no añade
> dependencias, pero la aplicación debe saber extraer el texto.

No hay `Rpdf_view_get_text(page)`. Sin él no se puede:

- buscar dentro del PDF (la función "Find" de cualquier lector),
- seleccionar y copiar texto,
- implementar el inverso de SyncTeX fino por palabra (LocalTeX lo hace por
  caja, que es menos preciso),
- enlazar un clic a una referencia bibliográfica.

PDFium ya expone `FPDFText_*`; el trabajo es envolverlo (contar caracteres,
rectángulos por carácter, índice de carácter bajo un punto). Es la carencia
más importante del visor.

### 4.2 Seguir la página que el usuario mira

Hay `get_page` (la página "de arriba") y `get_visible_first_page` /
`get_visible_last_page`, así que una barra de estado puede mostrar el rango
visible. Lo que falta es **un aviso**: no hay callback de "la página visible
cambió", así que la aplicación tiene que consultarlo en cada frame si quiere
mantener un número de página o un índice sincronizado. Un
`Rpdf_view_set_page_changed_callback` ahorraría ese sondeo.

> **Estado.** Hecho: `Rpdf_view_set_page_changed_callback`. Todas las
> asignaciones de la página actual pasan por `rvn_pdf_set_page` en
> `src/widgets/pdf_view.c`, que avisa solo si cambia. Test: `test_page_changed`
> en `tests/test_pdf_view.c`. El aviso puede llegar durante el layout (cuando la
> página se corrige al cambiar el número de páginas); la aplicación no debe
> reentrar en el visor en ese callback.

Tampoco hay un control de "ir a la página N": la aplicación lo construye con un
`TextInput` y `set_page`, que está bien, pero es el tipo de cosa que el widget
podría dar hecha si el visor quiere ser un lector.

### 4.3 Historial de navegación

Un lector necesita "atrás" y "adelante" (saltaste a una referencia, quieres
volver). Eso lo puede hacer la aplicación, pero el widget tendría que avisar
de los saltos que no son del usuario (scroll por SyncTeX) para no ensuciar el
historial.

### 4.4 Enlaces internos y externos del PDF

No hay callback de "se hizo clic en un enlace". Un PDF de LaTeX con `hyperref`
tiene cientos; hoy el clic llega como clic en una coordenada y la aplicación
no sabe distinguir un enlace de un texto.

---

## 5. P3: editor de texto, detalles

- **Búsqueda sin resultado por patrón inválido.** `Rtext_editor_set_search`
  devuelve `R_FALSE` y emite `R_ERROR_INVALID_VALUE`, pero no dice *qué* parte
  del patrón falló. La aplicación solo puede mostrar "no válido". Un
  `Rtext_editor_get_search_error` con el mensaje del compilador de regex
  ayudaría.
- **Sin reemplazo con grupos desde la API de alto nivel.**
  `Rtext_editor_expand_replacement` existe y funciona, pero es estático: hay
  que llamarlo por cada match. Un `Rtext_editor_replace_all` que acepte
  `$1` directamente (como hace el de la interfaz) ahorraría el bucle.
- **Marcadores sin "dueño".** Hecho: `Rtext_editor_add_marker_from` y
  `Rtext_editor_clear_markers_of_source` (ver §5 abajo).
- (Texto original:) `Rtext_editor_clear_markers` borra todos. Si la
  aplicación tiene dos fuentes de marcadores (el log del compilador y el
  corrector ortográfico, como LocalTeX) tiene que reconstruir ambos cada vez
  que una cambia. Un `clear_markers_of_source(id)` sería más simple y más
  rápido.
- **Sin acceso al ancho de una línea en píxeles** ni a la posición en pantalla
  de una columna; para tooltips alineados o minimapas hace falta calcularlo
  fuera.

---

## 6. P3: proceso, archivos, ajustes

- **`Rprocess_find` devuelve un puntero a un buffer estático** ("valid until
  the next call"). LocalTeX copia el valor inmediatamente, pero es una trampa
  fácil: dos `find` seguidos y el primero queda pisado. Un `Rprocess_find_into`
  o una cadena que la aplicación libere sería más seguro.

  > **Estado.** Hecho: `Rprocess_find_copy(program, &size)` devuelve una copia
  > que la aplicación libera con `Rmemory_free` (la misma forma que `Rfile_read`,
  > que es lo que los generadores de bindings soportan). Un primer intento como
  > `Rprocess_find_into(program, buffer, size)` no se pudo generar: no hay
  > soporte para un `char*` de buffer de entrada. Test en `tests/test_process.c`.
- **`Rfile_dialog_*` devuelve un puntero global** (`g_dialogs.paths[0]`) que
  se invalida en el siguiente diálogo. Mismo problema, misma solución.
- **No hay `Rfile_copy`, `Rfile_move` ni `Rfile_mkdir_p`** en la API de
  archivos: LocalTeX usa `std::filesystem`, lo cual está bien, pero entonces
  `Rfile_read`/`Rfile_write` no aportan mucho (solo el registro de archivos en
  memoria, que sí es útil para los tests).
- **Ajustes sin tipos anidados (decisión tomada).** Se añade
  `Rsettings_remove_prefix(settings, "project.recent")` para borrar una lista entera.
  No se añaden `set_array`/`get_array`: la forma de clave por entrada ya la usa
  LocalTeX, y el problema real era que las claves sobrantes quedaban (esto lo
  resuelve). Test: `test_remove_prefix` en `tests/test_settings.c`.
- (Texto original:) **Ajustes sin tipos anidados.** `Rsettings_*` es plano (clave → valor). Para
  una lista (los proyectos recientes) hay que inventar `project.recent.0`,
  `project.recent.1`… y acordarse de borrar los sobrantes; LocalTeX lo hace en
  `settings.cpp` con un bucle que borra las claves pasadas. Existe
  `Rsettings_set_json`/`get_json` como escape, pero entonces se pierde la
  comodidad de las `get_text`/`set_text` y hay que construir el JSON a mano.
  Un `Rsettings_set_array`/`get_array` (o un `Rsettings_remove_prefix`) haría
  el patrón obvio.
- **Sin "guardar si cambió".** `Rsettings_save` escribe siempre; las
  aplicaciones llaman a `save()` con frecuencia (LocalTeX lo hace en cada
  cambio de opción) y no hay forma de saber si el JSON cambió. Es barato
  comparar la cadena serializada antes de escribir, pero la información la
  tiene la biblioteca, no la aplicación.

---

## 7. P3: documentación, ejemplos y build

- **Tres fuentes de verdad.** `docs/reference.html` (generado de las
  cabeceras), los comentarios de las cabeceras y `docs/IMPLEMENTADO.md`. El
  registro (`IMPLEMENTADO.md`) no menciona `Rprocess_destroy` —normal, se
  añadió en esta revisión— pero tampoco dice de qué fecha es, así que no se
  sabe si está al día. `reference.html` sí sale de las cabeceras, con lo que
  está más cerca de la verdad que el registro.
- **`docs/XML.md` está bien y sí cubre el fork**: documenta `PdfView` con su
  `Background` y el resto de atributos que añadió `localtex`. Es la mejor
  documentación del proyecto.
- **Sin CHANGELOG (hecho).** `docs/CHANGELOG.md`, formato Keep a Changelog, con
  los commits del fork y los cambios de esta ronda en «Sin publicar».
- (Texto original:) **Sin CHANGELOG.** Para un fork que diverge de upstream, saber qué cambió
  entre dos revisiones del fork es lo primero que necesita quien integra.
- **`RAVEN_BUILD_TESTS` arranca `bindings_up_to_date`**, que falla de forma
  crónica (ver §1.2). Hay CI (`.github/workflows/build.yml` corre `ctest` en
  Linux headless en cada push), así que **el CI está rojo ahora mismo**: los
  tres P0 de §1 lo tumban. Un rojo crónico es peor que ninguno: enseña a
  ignorar el semáforo.
- **El binding Python no se prueba en Windows** y el de C# no tiene prueba en
  ninguna plataforma (`bindings/CMakeLists.txt` lo dice). Son los dos que más
  se usan para prototipar.
- **Faltan ejemplos de la API de PDF.** El bucle documento → `set_page_count`
  → `set_page_size` → callback → `set_page_bitmap` está en el comentario de
  cabecera, pero no hay un ejemplo compilable, y es la parte más fácil de
  hacer mal (LocalTeX tardó en descubrir que una textura liberada no se vuelve
  a pedir hasta que se tocó `pdf_view.c`).

---

## 8. Lo que ya está bien y conviene no romper

Para que la lista anterior no dé una impresión equivocada:

- **La API es coherente y plana.** `R<objeto>_<acción>`, handles opacos,
  convención de nombres estable. Eso es lo que hace que un binding se pueda
  generar y que LocalTeX se lea bien.
- **La separación núcleo / widgets / render es real.** El backend nulo permite
  probar toda la interfaz sin GPU ni display; es la razón por la que la
  batería de LocalTeX corre en CI sin pantalla.
- **La documentación en las cabeceras es excelente**: cada función dice si el
  puntero caduca, si el callback corre en el hilo de UI, qué devuelve en el
  caso raro. Es mejor que la media de las bibliotecas C.
- **El parseo de logs de LaTeX (`Rlatex_log_parse`) y el editor de texto**
  (plegado, snippets, multi-cursor, búsqueda regex) son completos y funcionan
  bien bajo uso real. LocalTeX no tuvo que tocar nada de eso.
- **`test_process`, `test_text_editor`, `test_latex_editor`, `test_pdf_view`**
  existen y pasan: la parte que más usa LocalTeX está cubierta.

---

## 9. Orden sugerido

1. **Arreglar los tres P0** y poner `ctest` en CI. Sin eso, cualquier cambio
   posterior es a ciegas.
2. **Generación de handles más ancha** (§2.1) y `Rprocess_destroy` público
   (§2.2): son la diferencia entre una biblioteca de demo y una de aplicación.
3. **Estado de backend consultable** (§2.3) y saber qué texturas quedaron
   vacías tras recuperar el dispositivo (§2.4).
4. **Las cinco operaciones de editor** de §3: son pequeñas, ya existen
   internamente, y cada una ahorra una duplicación en la aplicación.
5. **Texto del PDF** (§4.1): es el trabajo grande que más valor añade al
   visor.
6. **CHANGELOG y regenerar bindings** como parte del flujo de release.

---

## 10. Cómo se hizo esta revisión

Para que los números se puedan repetir:

```sh
# la biblioteca y su suite
cmake -S . -B /tmp/raven-build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAVEN_BUILD_TESTS=ON
cmake --build /tmp/raven-build -j
ctest --test-dir /tmp/raven-build --output-on-failure     # 29/32

# el segfault, con la pila
gdb -batch -ex run -ex "bt 20" --args /tmp/raven-build/bin/test_render

# el desfase de los bindings
python3 tools/bindgen/bindgen.py --check                   # exit 1

# el handle que revive (30 líneas, ver §2.1)
gcc -std=c11 gen.c -Iinclude -L/tmp/raven-build -lraven -o gen
LD_LIBRARY_PATH=/tmp/raven-build ./gen                     # "YES (bug)"
```

Lo que **no** se probó: Windows y macOS (el CI los compila, pero esta revisión
fue en Linux); el visualizador de PDF contra un driver distinto de PDFium; el
camino de Wayland; y las pruebas de navegador (`test_web`), que necesitan
Chrome.

Ronda de revisión posterior: `bindgen.py` y `gen_reference_entries.py`
regenerados; `bindings/README.md` corregido a C++14; `test_render` sigue en
rojo; tests nuevos de handles y de ediciones del editor.

`designer_folder` (tools/CMakeLists.txt) fallaba de forma intermitente. **Arreglado**:
el designer detectaba un cambio de archivo solo por la fecha de modificación, y el
reloj del sistema es más grueso que dos escrituras seguidas, así que dos escrituras
con el mismo tiempo se tomaban por una. Ahora también compara el tamaño
(`tools/designer/files.c`, `ds_file_size` en `os.c`). Pasa 5 de 5 ejecuciones seguidas.

Lo que se leyó y no se ejecutó: los puntos marcados como "por lectura" en la
tabla de §0, y los de §2.3, §2.4 y §3 (la ausencia de una función se comprobó
con `grep`; el impacto en una aplicación se razona, no se mide).
