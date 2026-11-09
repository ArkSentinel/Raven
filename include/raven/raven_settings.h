/**
 * Raven
 * raven_settings.h : a persistent store of settings, backed by a JSON file.
 *
 * An application keeps what it must remember between runs here: the last
 * project, the theme of each project, the paths of the tools it calls, the
 * files opened lately. The store is a JSON object of flat keys:
 *
 *   {
 *     "editor.font_size": 13,
 *     "project.theme": "paper",
 *     "tools.latexmk": "/usr/bin/latexmk",
 *     "recent": ["main.tex", "notes.tex"]
 *   }
 *
 * The values are booleans, integers, numbers and texts. Nested objects are
 * accepted when reading and flattened to dotted keys ("a": {"b": 1} becomes
 * "a.b"), so a hand-written file may be nested; "//" comments and trailing
 * commas are tolerated too, which makes the file nice to edit.
 *
 * The same object also reads and writes a whole JSON document, arrays and
 * objects included, for the project files of an application (a file such as
 * "project.json", and the like).
 */

#ifndef RAVEN_SETTINGS_H
#define RAVEN_SETTINGS_H

#include "raven_core.h"


/** What a key holds. */
typedef enum RSettingType{
	/** No such key, or a value that is an object, an array or null. */
	R_SETTING_NONE = 0x0300,
	R_SETTING_BOOL,
	R_SETTING_INT,
	R_SETTING_NUMBER,
	R_SETTING_TEXT
}RSettingType;


#ifdef __cplusplus
extern "C" {
#endif

	/* -----------------------------------------------------------------------
	 *  The store
	 * --------------------------------------------------------------------- */

	/**
	 * @brief Opens a settings store.
	 * @param path File the store is read from and written to, or NULL for a
	 * store that lives in memory only. The file is read when it exists.
	 * @return The store, or R_INVALID_ID (after reporting an error).
	 */
	RAVEN_API Rid Rsettings_create(const char* path);
	/** @brief Frees a store. Rshutdown frees the stores the application left alive. */
	RAVEN_API void Rsettings_destroy(Rid settings);

	/**
	 * @brief Reads the file of the store again, replacing everything it holds.
	 * @return R_TRUE when the file existed and was valid JSON. When it did not,
	 * the store is emptied (a store with no file stays empty).
	 */
	RAVEN_API int Rsettings_load(Rid settings);

	/**
	 * @brief Writes the store to its file: a temporary file next to it, then a
	 * rename, so a reader never sees half a file. The folder is created when
	 * it does not exist.
	 * @return R_FALSE when the store has no file or when the file could not be
	 * written.
	 */
	RAVEN_API int Rsettings_save(Rid settings);

	/** @brief Path of the file of the store ("" for a store that lives in memory only). */
	RAVEN_API const char* Rsettings_get_path(Rid settings);

	/* -----------------------------------------------------------------------
	 *  Values
	 * --------------------------------------------------------------------- */

	/** @return What the key holds, R_SETTING_NONE when it is not there (or holds an object, an array or null). */
	RAVEN_API RSettingType Rsettings_get_type(Rid settings, const char* key);

	/** @brief The value of the key; `fallback` when it is missing or holds another type. */
	RAVEN_API int Rsettings_get_bool(Rid settings, const char* key, int fallback);
	RAVEN_API int Rsettings_get_int(Rid settings, const char* key, int fallback);
	RAVEN_API double Rsettings_get_number(Rid settings, const char* key, double fallback);
	/** @brief Valid until the store changes. NULL only when the key is missing and `fallback` is NULL. */
	RAVEN_API const char* Rsettings_get_text(Rid settings, const char* key, const char* fallback);

	RAVEN_API void Rsettings_set_bool(Rid settings, const char* key, int value);
	RAVEN_API void Rsettings_set_int(Rid settings, const char* key, int value);
	RAVEN_API void Rsettings_set_number(Rid settings, const char* key, double value);
	/** @brief An empty text stores ""; a NULL text removes the key. */
	RAVEN_API void Rsettings_set_text(Rid settings, const char* key, const char* value);

	/** @brief Removes a key. R_TRUE when it was there. */
	RAVEN_API int Rsettings_remove(Rid settings, const char* key);
	/**
	 * @brief Removes every key whose full name starts with `prefix` (a whole list, such as
	 * "project.recent", with the keys it had). Returns how many keys were removed.
	 */
	RAVEN_API int Rsettings_remove_prefix(Rid settings, const char* prefix);
	/** @brief R_TRUE when the key is there, whatever it holds. */
	RAVEN_API int Rsettings_has(Rid settings, const char* key);

	/* -----------------------------------------------------------------------
	 *  Whole documents
	 * --------------------------------------------------------------------- */

	/**
	 * @brief The whole document as text, for a project file that holds arrays
	 * and objects too. Valid until the store changes or the function is called
	 * again. "" when the store is empty.
	 */
	RAVEN_API const char* Rsettings_get_json(Rid settings);

	/**
	 * @brief Replaces the whole document.
	 * @return R_FALSE when `json` is not valid JSON: the store is not changed.
	 */
	RAVEN_API int Rsettings_set_json(Rid settings, const char* json);

	/**
	 * @brief How many keys the object at `prefix` has ("" for the whole
	 * document), or how many items the array at `prefix` has. 0 when there is
	 * nothing there.
	 */
	RAVEN_API int Rsettings_get_key_count(Rid settings, const char* prefix);

	/**
	 * @brief The index-th key of the object at `prefix` ("" for the whole
	 * document), or the index-th item of an array as "0", "1"... NULL when
	 * `index` is out of range. Valid until the next call of this function
	 * that returns a key.
	 */
	RAVEN_API const char* Rsettings_get_key(Rid settings, const char* prefix, int index);

	/* -----------------------------------------------------------------------
	 *  The usual folders
	 * --------------------------------------------------------------------- */

	/**
	 * @brief The folder where the settings of the application live:
	 * ~/.config/<app> ($XDG_CONFIG_HOME honoured),
	 * ~/Library/Application Support/<app> on macOS,
	 * %APPDATA%\<app> on Windows. Created when it does not exist; valid until
	 * the next call from the same thread.
	 */
	RAVEN_API const char* Rsettings_get_config_folder(const char* app_name);
	/** @brief The folder for what can be made again (caches), by the same rules. */
	RAVEN_API const char* Rsettings_get_cache_folder(const char* app_name);

	/**
	 * @brief Opens <config folder>/<file_name> for the application, creating
	 * the folder when needed. Rsettings_open("myeditor", "sessions.json").
	 * @return R_INVALID_ID when the folder or the file cannot be opened.
	 */
	RAVEN_API Rid Rsettings_open(const char* app_name, const char* file_name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RAVEN_SETTINGS_H */
