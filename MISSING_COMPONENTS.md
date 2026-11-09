# Componentes faltantes para LocalTeX (fork de Raven)

Documento de planificación para el fork `localtex` de Raven.  
Lista todos los componentes que Raven no provee y que LocalTeX necesita, con prioridad, complejidad y estrategia de implementación.

## Leyenda

- **Prioridad:** P0 (bloqueante para MVP), P1 (core), P2 (avanzado), P3 (opcional).
- **Complejidad:** Baja / Media / Alta.
- **Donde:** Fork (código C en Raven) o Externo (dependencia/proceso).

---

## 1. UI (Raven)

### 1.1 Widget de preview PDF
- **Prioridad:** P0
- **Complejidad:** Alta
- **Donde:** Fork + Externo (PDFium)
- **Descripción:** Widget que renderiza páginas PDF a imágenes y las muestra con zoom/scroll. Raven no tiene visor PDF ni webview (issue #57).
- **Estrategia:** Implementar sobre `R_WIDGET_IMAGE`/canvas + PDFium (Apache-2.0) para renderizar páginas a texturas. En macOS se puede usar Quartz como alternativa nativa.
- **SyncTeX:** requiere mapeo de coordenadas (forward/inverse).

### 1.2 Desactivar/parchear animaciones laggy
- **Prioridad:** P0
- **Complejidad:** Media
- **Donde:** Fork
- **Descripción:** Issue #20: animaciones de highlight y resize hacen la UI sentirse lenta. Desactivar o hacer instantáneas por defecto.
- **Estrategia:** Parche en el renderer para deshabilitar animaciones de movimiento de highlight bar y resize de ventana.

### 1.3 Backdrops en Windows
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Fork
- **Descripción:** Issue #53: backdrops se vuelven blancos al hacer screenshot en Windows.
- **Estrategia:** Vigilar en builds Windows; parchear si afecta.

### 1.4 Temas persistentes por proyecto
- **Prioridad:** P2
- **Complejidad:** Baja
- **Donde:** Fork + Externo (config)
- **Descripción:** Guardar preferencia de tema por proyecto.
- **Estrategia:** Almacenar en config local (JSON) y aplicar al cargar.

---

## 2. Editor de código (Raven text_editor)

### 2.1 Snippets LaTeX
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Fork o Externo (Python)
- **Descripción:** Atajos para `\begin{}`/`\end{}`, `\section{}`, etc.
- **Estrategia:** Implementar en la capa de abstracción UI (Python) usando `Rtext_editor_insert_text`; opcionalmente parchear Raven para snippets nativos.

### 2.2 Code folding por entorno
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Fork
- **Descripción:** Colapsar bloques `\begin{}`/`\end{}`.
- **Estrategia:** Parchear el text_editor para soportar folding por delimitadores.

### 2.3 Multi-cursor
- **Prioridad:** P2
- **Complejidad:** Alta
- **Donde:** Fork
- **Descripción:** Verificar si Raven text_editor lo soporta; si no, implementar.
- **Estrategia:** Revisar API de Raven; parchear si falta.

### 2.4 Búsqueda/reemplazo en archivo
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Fork o Externo
- **Descripción:** Barra de find/replace con regex.
- **Estrategia:** Implementar en capa UI usando API de text_editor; parchear Raven si se necesita rendimiento.

### 2.5 Go-to-definition (`\input`, `\include`)
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** Navegación entre archivos del proyecto.
- **Estrategia:** Resolver rutas en Python y usar `Rtext_editor_set_cursor` + abrir archivo.

### 2.6 Autocompletado de comandos/citas/ref
- **Prioridad:** P1
- **Complejidad:** Alta
- **Donde:** Externo (Python) + Fork (UI)
- **Descripción:** IntelliSense LaTeX: comandos, claves bibliográficas, etiquetas.
- **Estrategia:** Generar lista de candidatos en Python; usar `Rtext_editor_add_completion` de Raven.

### 2.7 Marcadores de error (desde log)
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Externo (Python) + Fork (UI)
- **Descripción:** Marcar líneas con error en el editor a partir del parseo del log.
- **Estrategia:** Usar `Rtext_editor_add_marker` de Raven.

### 2.8 Conteo de palabras
- **Prioridad:** P3
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Estadísticas del documento.
- **Estrategia:** Contar en Python y mostrar en status bar.

---

## 3. Compilación (externo)

### 3.1 Detección de TeX externo
- **Prioridad:** P0
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Buscar `pdflatex`/`latexmk` en PATH y rutas conocidas (MacTeX, TeX Live).
- **Estrategia:** Implementar en Python con `shutil.which` y rutas por plataforma.

### 3.2 Invocación de compilación
- **Prioridad:** P0
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** Ejecutar `latexmk` como proceso, capturar stdout/stderr.
- **Estrategia:** `subprocess` con streaming de salida al panel de log.

### 3.3 Parseo de log LaTeX
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** Extraer errores/warnings y línea afectada.
- **Estrategia:** Regex sobre el log; generar lista de marcadores.

### 3.4 Selección de motor
- **Prioridad:** P1
- **Complejidad:** Baja
- **Donde:** Externo (Python) + UI
- **Descripción:** pdfLaTeX/XeLaTeX/LuaLaTeX.
- **Estrategia:** Parámetro `-pdf`/`-xelatex`/`-lualatex` a latexmk.

### 3.5 Compilación en segundo plano
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** No bloquear la UI durante la compilación.
- **Estrategia:** Hilo o proceso asíncrono con callback al finalizar.

---

## 4. Vista previa PDF (externo + fork)

### 4.1 Renderizado de páginas a imagen
- **Prioridad:** P0
- **Complejidad:** Alta
- **Donde:** Externo (PDFium) o Externo (MuPDF CLI)
- **Descripción:** Convertir páginas PDF a PNG para mostrar en el widget image.
- **Estrategia:** Usar PDFium (Apache-2.0) embebido o `pdftoppm` (Poppler) como CLI. Evitar MuPDF (AGPL) salvo licencia comercial.

### 4.2 Visor de páginas (zoom, scroll)
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Fork (widget) + Externo
- **Descripción:** Navegación entre páginas, zoom, scroll.
- **Estrategia:** Implementar sobre canvas/image con controles de zoom.

### 4.3 SyncTeX (forward)
- **Prioridad:** P2
- **Complejidad:** Alta
- **Donde:** Externo (Python) + Fork (UI)
- **Descripción:** Saltar de fuente a PDF.
- **Estrategia:** Usar `synctex` (incluido en TeX Live) para obtener coordenadas; desplazar el visor.

### 4.4 SyncTeX (inverse)
- **Prioridad:** P2
- **Complejidad:** Alta
- **Donde:** Externo (Python) + Fork (UI)
- **Descripción:** Click en PDF → línea de fuente.
- **Estrategia:** Usar `synctex` para obtener archivo/línea; abrir en editor.

### 4.5 Recarga automática al compilar
- **Prioridad:** P1
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Detectar cambio del PDF y recargar el preview.
- **Estrategia:** File watcher o polling del mtime.

---

## 5. Gestión de proyectos y archivos

### 5.1 Árbol de archivos
- **Prioridad:** P1
- **Complejidad:** Baja
- **Donde:** Raven (ya disponible)
- **Descripción:** `R_WIDGET_TREE_VIEW` + `R_WIDGET_FILE_BROWSER`.
- **Estrategia:** Usar directamente.

### 5.2 Crear/renombrar/eliminar archivos
- **Prioridad:** P1
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Operaciones de filesystem.
- **Estrategia:** `os`/`pathlib` en Python + refrescar árbol.

### 5.3 Proyecto multi-archivo
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** Resolver `\input`/`\include` para navegación.
- **Estrategia:** Parsear el .tex principal.

### 5.4 Plantillas de proyecto
- **Prioridad:** P2
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Biblioteca local de plantillas.
- **Estrategia:** Copiar desde `~/.localtex/templates`.

### 5.5 Importar/exportar proyecto (.zip)
- **Prioridad:** P2
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Para sync con Overleaf.
- **Estrategia:** `zipfile`.

### 5.6 Detección de cambios externos
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** File watcher para recargar.
- **Estrategia:** `watchdog` o polling.

---

## 6. Bibliografía

### 6.1 Editor de .bib
- **Prioridad:** P2
- **Complejidad:** Baja
- **Donde:** Raven (text_editor con lenguaje bibtex)
- **Descripción:** Syntax highlighting BibTeX.
- **Estrategia:** `Rtext_editor_set_language(ed, "bibtex")` si soportado.

### 6.2 Integración Zotero/JabRef
- **Prioridad:** P3
- **Complejidad:** Media
- **Donde:** Externo
- **Descripción:** Leer .bib o invocar gestor.
- **Estrategia:** Monitorear cambios en el .bib.

### 6.3 Autocompletado de citas
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** Candidatos desde .bib.
- **Estrategia:** Parsear .bib y alimentar `Rtext_editor_add_completion`.

---

## 7. Colaboración local

### 7.1 CRDT
- **Prioridad:** P3
- **Complejidad:** Alta
- **Donde:** Externo (Python: Yjs/Automerge bindings)
- **Descripción:** Sincronización local/LAN.
- **Estrategia:** Integrar librería CRDT en Python.

### 7.2 WebSocket local
- **Prioridad:** P3
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** Servidor embebido para peers.
- **Estrategia:** `websockets` o `asyncio`.

### 7.3 Presencia (cursores)
- **Prioridad:** P3
- **Complejidad:** Alta
- **Donde:** Fork + Externo
- **Descripción:** Mostrar cursores de otros usuarios.
- **Estrategia:** Parchear text_editor para renderizar cursores remotos.

### 7.4 Historial/versionado local
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Externo (Git)
- **Descripción:** Auto-commit por guardado.
- **Estrategia:** Invocar git en Python.

---

## 8. Control de versiones (Git)

### 8.1 Invocación de git
- **Prioridad:** P2
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Proceso externo.
- **Estrategia:** `subprocess`.

### 8.2 Diff visual
- **Prioridad:** P3
- **Complejidad:** Media
- **Donde:** Fork o Externo
- **Descripción:** Mostrar diff en la UI.
- **Estrategia:** Opcional.

### 8.3 Auto-commit por guardado
- **Prioridad:** P3
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Commit automático.
- **Estrategia:** Opcional, configurable.

---

## 9. Corrector ortográfico

### 9.1 Hunspell integrado
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Externo (Python: `pyhunspell` o `hunspell` CLI)
- **Descripción:** Diccionarios locales.
- **Estrategia:** Invocar hunspell o usar binding Python.

### 9.2 Subrayado de errores
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Fork (UI) + Externo
- **Descripción:** Usar `Rtext_editor_add_marker` para subrayar.
- **Estrategia:** Marcar palabras incorrectas.

---

## 10. Configuración y ajustes

### 10.1 Preferencias (UI)
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Fork + Externo
- **Descripción:** Ventana de ajustes.
- **Estrategia:** Implementar con widgets de Raven.

### 10.2 Keybindings personalizables
- **Prioridad:** P2
- **Complejidad:** Media
- **Donde:** Fork
- **Descripción:** Mapeo de teclas.
- **Estrategia:** Raven soporta shortcuts (`Raction_bind`); hacerlos configurables.

### 10.3 Configuración por proyecto
- **Prioridad:** P2
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Archivo `.localtex.json` por proyecto.
- **Estrategia:** JSON.

### 10.4 Rutas de TeX/PDFium
- **Prioridad:** P0
- **Complejidad:** Baja
- **Donde:** Externo (Python)
- **Descripción:** Configurar rutas de herramientas externas.
- **Estrategia:** Detección automática + override manual.

---

## 11. IA opcional

### 11.1 Integración Ollama
- **Prioridad:** P3
- **Complejidad:** Media
- **Donde:** Externo (Python)
- **Descripción:** LLM local.
- **Estrategia:** HTTP a Ollama local.

### 11.2 Autocompletado IA
- **Prioridad:** P3
- **Complejidad:** Alta
- **Donde:** Externo (Python) + Fork (UI)
- **Descripción:** Sugerencias contextuales.
- **Estrategia:** Opcional.

### 11.3 Corrección de errores de compilación
- **Prioridad:** P3
- **Complejidad:** Alta
- **Donde:** Externo (Python)
- **Descripción:** Sugerir fixes desde el log.
- **Estrategia:** Opcional.

---

## 12. Terminal integrado (opcional)

### 12.1 Terminal embebido
- **Prioridad:** P3
- **Complejidad:** Alta
- **Donde:** Fork
- **Descripción:** Terminal dentro de la UI.
- **Estrategia:** Opcional; útil para usuarios avanzados.

---

## 13. Empaquetado y distribución

### 13.1 Build multiplataforma
- **Prioridad:** P0
- **Complejidad:** Media
- **Donde:** CI (GitHub Actions)
- **Descripción:** Linux/Windows/macOS.
- **Estrategia:** CMake + CI.

### 13.2 Bundle Gumroad/itch.io (con TeX)
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Empaquetado
- **Descripción:** Incluir TeX Live (cumplir licencias).
- **Estrategia:** Instalador con TeX opcional.

### 13.3 Mac App Store (sin TeX)
- **Prioridad:** P1
- **Complejidad:** Alta
- **Donde:** Empaquetado
- **Descripción:** Requiere MacTeX instalado por el usuario.
- **Estrategia:** Notarización, firmado.

### 13.4 Updater
- **Prioridad:** P3
- **Complejidad:** Media
- **Donde:** Externo
- **Descripción:** Actualizaciones automáticas.
- **Estrategia:** Opcional.

### 13.5 Firmado de código
- **Prioridad:** P1
- **Complejidad:** Media
- **Donde:** Empaquetado
- **Descripción:** Requerido para App Store.
- **Estrategia:** Certificados de desarrollador.

---

## Resumen de componentes faltantes (bloqueantes para MVP)

1. Preview PDF (PDFium) — P0.
2. Desactivar animaciones laggy — P0.
3. Detección de TeX externo — P0.
4. Invocación de compilación — P0.
5. Parseo de log — P1 (pero necesario para UX).
6. Snippets/folding/autocompletado — P1.
7. Árbol de archivos (ya disponible) — P1.
8. Configuración de rutas — P0.

## Notas

- Todo se encapsula detrás de una capa de abstracción UI para poder cambiar de toolkit (Qt 6) sin reescribir el core.
- Sin tecnologías web; sin emojis.
- Licencias: Raven (Apache-2.0), PDFium (Apache-2.0), CRDT (MIT), Ollama (MIT). TeX externo (usuario instala).
- Issues de Raven a vigilar: #57 (PDF), #20 (animaciones), #53 (backdrops Windows).
