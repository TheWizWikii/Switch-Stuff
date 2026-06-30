// switch-updater - homebrew launcher/menu that downloads files from a github-hosted JSON
// Build with devkitPro + libnx + SDL2 + jansson + libcurl

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <jansson.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <zzip/zzip.h>

// ---------- CONFIG: change this to your own raw github json url ----------
#define JSON_URL "https://raw.githubusercontent.com/TheWizWikii/Switch-Stuff/main/apps.json"
#define FONT_PATH "romfs:/fonts/font.ttf"
#define SCREEN_W 1280
#define SCREEN_H 720
#define LOG_PATH "/switch-updater-debug.log"

// ---------- simple file logger (writes to root of sd card so it's easy to find) ----------
#include <stdarg.h>
static FILE *g_log = NULL;
static void log_open(void) { g_log = fopen(LOG_PATH, "w"); }
static void log_close(void) { if (g_log) fclose(g_log); }
static void dlog(const char *fmt, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fprintf(g_log, "\n");
    fflush(g_log);
}

// ---------- data structures ----------
typedef struct {
    char name[128];
    char url[512];
    char dest[256];
    bool extract;
    int strip;
} Item;

typedef struct {
    char name[64];
    Item items[64];
    int item_count;
} Section;

static Section sections[8];
static int section_count = 0;

// ---------- curl: download into memory buffer ----------
struct MemBuf {
    char *data;
    size_t size;
};

static size_t mem_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemBuf *mem = (struct MemBuf *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

// download URL into memory, returns malloc'd buffer (caller frees) or NULL
static char *download_to_memory(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    struct MemBuf mem = { .data = malloc(1), .size = 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&mem);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "switch-updater/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // switch certs can be finicky; tighten if you set up CA bundle

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(mem.data);
        return NULL;
    }
    return mem.data;
}

// progress callback just used to redraw a simple bar while downloading a file
typedef struct { SDL_Renderer *renderer; TTF_Font *font; const char *label; } ProgressCtx;

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    ProgressCtx *ctx = (ProgressCtx *)clientp;
    SDL_Renderer *r = ctx->renderer;

    SDL_SetRenderDrawColor(r, 20, 20, 30, 255);
    SDL_RenderClear(r);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(ctx->font, ctx->label, white);
    if (surf) {
        SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_Rect dst = { 80, 280, surf->w, surf->h };
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
        SDL_FreeSurface(surf);
    }

    if (dltotal > 0) {
        float pct = (float)dlnow / (float)dltotal;
        SDL_Rect barBg = { 80, 340, SCREEN_W - 160, 40 };
        SDL_SetRenderDrawColor(r, 60, 60, 70, 255);
        SDL_RenderFillRect(r, &barBg);
        SDL_Rect barFg = { 80, 340, (int)((SCREEN_W - 160) * pct), 40 };
        SDL_SetRenderDrawColor(r, 90, 200, 120, 255);
        SDL_RenderFillRect(r, &barFg);
    }

    SDL_RenderPresent(r);

    // pump events so the switch OS doesn't think we're frozen
    SDL_Event e;
    while (SDL_PollEvent(&e)) {}

    return 0; // returning non-zero aborts the transfer
}

// download a URL straight to a file on the sd card, with progress UI
static bool download_to_file(const char *url, const char *dest_path, SDL_Renderer *renderer, TTF_Font *font) {
    dlog("download_to_file: inicio, dest=%s", dest_path);

    // make sure parent dir exists (mkdir -p style, single level is usually enough for /switch/Name/)
    char dir[256];
    strncpy(dir, dest_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        dlog("  creando directorio: %s", dir);
        int mres = mkdir(dir, 0777); // best-effort, ignore errors if it already exists
        dlog("  mkdir devolvio: %d (errno=%d)", mres, errno);
    }

    dlog("  abriendo fopen: %s", dest_path);
    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        dlog("  ERROR: fopen devolvio NULL (errno=%d)", errno);
        return false;
    }
    dlog("  fopen OK");

    CURL *curl = curl_easy_init();
    if (!curl) {
        dlog("  ERROR: curl_easy_init devolvio NULL");
        fclose(fp);
        return false;
    }
    dlog("  curl_easy_init OK, comenzando descarga...");

    ProgressCtx ctx = { renderer, font, dest_path };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "switch-updater/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    dlog("  curl_easy_perform devolvio: %d (%s)", res, curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    fclose(fp);

    return res == CURLE_OK;
}

// ---------- zip extraction (using zziplib) ----------

// mkdir -p style recursive directory creation
static void ensure_dir_recursive(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

// skips the first `n` path components of a zip entry name.
// e.g. skip_components("minimal/minimal/file.txt", 2) -> "file.txt"
static const char *skip_path_components(const char *name, int n) {
    const char *p = name;
    for (int i = 0; i < n; i++) {
        const char *slash = strchr(p, '/');
        if (!slash) return p; // nothing left to strip, return as-is
        p = slash + 1;
    }
    return p;
}

// extracts every file inside zip_path into dest_dir, preserving subfolders.
// `strip` discards that many leading path components from each entry (like tar --strip-components).
// shows a simple "Extrayendo: ..." screen while it works.
static bool extract_zip(const char *zip_path, const char *dest_dir, int strip, SDL_Renderer *renderer, TTF_Font *font) {
    dlog("extract_zip: %s -> %s (strip=%d)", zip_path, dest_dir, strip);
    ensure_dir_recursive(dest_dir);

    ZZIP_DIR *zdir = zzip_dir_open(zip_path, NULL);
    if (!zdir) {
        dlog("  ERROR: zzip_dir_open fallo");
        return false;
    }

    ZZIP_DIRENT entry;
    int count = 0;

    while (zzip_dir_read(zdir, &entry)) {
        const char *rel_name = skip_path_components(entry.d_name, strip);
        if (rel_name[0] == 0) continue; // stripped down to nothing, skip

        char outpath[600];
        snprintf(outpath, sizeof(outpath), "%s/%s", dest_dir, rel_name);

        // redraw a simple progress screen so it's clear something is happening
        SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
        SDL_RenderClear(renderer);
        SDL_Color white = {255, 255, 255, 255};
        char label[300];
        snprintf(label, sizeof(label), "Extrayendo: %s", rel_name);
        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, label, white);
        if (surf) {
            SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
            SDL_Rect dst = {80, 300, surf->w, surf->h};
            SDL_RenderCopy(renderer, tex, NULL, &dst);
            SDL_DestroyTexture(tex);
            SDL_FreeSurface(surf);
        }
        SDL_RenderPresent(renderer);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}

        // directory entries end with '/'
        size_t nlen = strlen(rel_name);
        if (nlen > 0 && rel_name[nlen - 1] == '/') {
            ensure_dir_recursive(outpath);
            continue;
        }

        // make sure the parent folder of this file exists (zips can have nested paths)
        char parent[600];
        strncpy(parent, outpath, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = 0;
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = 0;
            ensure_dir_recursive(parent);
        }

        ZZIP_FILE *zf = zzip_file_open(zdir, entry.d_name, 0);
        if (!zf) {
            dlog("  WARN: no se pudo abrir dentro del zip: %s", entry.d_name);
            continue;
        }

        FILE *out = fopen(outpath, "wb");
        if (!out) {
            dlog("  WARN: no se pudo crear: %s (errno=%d)", outpath, errno);
            zzip_file_close(zf);
            continue;
        }

        char buf[4096];
        zzip_ssize_t n;
        while ((n = zzip_file_read(zf, buf, sizeof(buf))) > 0) {
            fwrite(buf, 1, n, out);
        }
        fclose(out);
        zzip_file_close(zf);
        count++;
    }

    zzip_dir_close(zdir);
    dlog("extract_zip: %d archivos extraidos", count);
    return count > 0;
}

// ---------- json parsing ----------
static bool parse_sections_json(const char *jsonText) {
    json_error_t error;
    json_t *root = json_loads(jsonText, 0, &error);
    if (!root) return false;

    section_count = 0;
    const char *key;
    json_t *value;
    json_object_foreach(root, key, value) {
        if (section_count >= 8) break;
        if (!json_is_array(value)) continue;

        Section *sec = &sections[section_count];
        strncpy(sec->name, key, sizeof(sec->name) - 1);
        sec->item_count = 0;

        size_t idx;
        json_t *item_json;
        json_array_foreach(value, idx, item_json) {
            if (sec->item_count >= 64) break;
            json_t *jname = json_object_get(item_json, "name");
            json_t *jurl  = json_object_get(item_json, "url");
            json_t *jdest = json_object_get(item_json, "dest");
            json_t *jextract = json_object_get(item_json, "extract");
            json_t *jstrip = json_object_get(item_json, "strip");
            if (!json_is_string(jname) || !json_is_string(jurl) || !json_is_string(jdest)) continue;

            Item *it = &sec->items[sec->item_count];
            strncpy(it->name, json_string_value(jname), sizeof(it->name) - 1);
            strncpy(it->url, json_string_value(jurl), sizeof(it->url) - 1);
            strncpy(it->dest, json_string_value(jdest), sizeof(it->dest) - 1);
            it->extract = jextract && json_is_true(jextract);
            it->strip = (jstrip && json_is_integer(jstrip)) ? (int)json_integer_value(jstrip) : 0;
            sec->item_count++;
        }
        section_count++;
    }

    json_decref(root);
    return true;
}

// fetches JSON_URL fresh and re-parses it into the global `sections` array.
// returns true on success. used both at startup and for the manual refresh action.
static bool fetch_and_parse_json(void) {
    dlog("Descargando JSON desde: %s", JSON_URL);
    char *json_text = download_to_memory(JSON_URL);
    bool ok = false;
    if (json_text) {
        dlog("JSON descargado, %zu bytes", strlen(json_text));
        ok = parse_sections_json(json_text);
        dlog("Parseo JSON: %s, secciones encontradas: %d", ok ? "OK" : "FALLO", section_count);
        free(json_text);
    } else {
        dlog("ERROR: download_to_memory devolvio NULL (fallo de red o curl)");
    }
    return ok;
}

// ---------- rendering helpers ----------
static void draw_text(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y, SDL_Color color) {
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

static int text_width(TTF_Font *font, const char *text) {
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text, &w, &h);
    return w;
}

// draws text horizontally centered inside [x, x+w]
static void draw_text_centered(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int w, int y, SDL_Color color) {
    int tw = text_width(font, text);
    draw_text(renderer, font, text, x + (w - tw) / 2, y, color);
}

// simple filled circle (midpoint algorithm), used for the A/B button badges
static void draw_filled_circle(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)(SDL_sqrt((double)(radius * radius - dy * dy)) + 0.5);
        SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// draws a small round button badge (like the physical A/B Switch buttons) with a letter inside
static void draw_button_badge(SDL_Renderer *renderer, TTF_Font *font, int x, int y, int radius, const char *letter, SDL_Color bg) {
    draw_filled_circle(renderer, x, y, radius, bg);
    SDL_Color black = {20, 20, 20, 255};
    int tw = text_width(font, letter);
    draw_text(renderer, font, letter, x - tw / 2, y - radius + 4, black);
}

// ---------- main ----------
int main(int argc, char **argv) {
    romfsInit();
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    TTF_Init();

    SDL_Window *window = SDL_CreateWindow("switch-updater", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    log_open();
    dlog("App iniciada");
    dlog("SDL window: %p  renderer: %p", (void*)window, (void*)renderer);

    TTF_Font *font = TTF_OpenFont(FONT_PATH, 28);
    TTF_Font *fontBig = TTF_OpenFont(FONT_PATH, 38);
    TTF_Font *fontSmall = TTF_OpenFont(FONT_PATH, 20);
    if (!font || !fontBig || !fontSmall) {
        dlog("ERROR cargando fuente '%s': %s", FONT_PATH, TTF_GetError());
    } else {
        dlog("Fuente cargada correctamente");
    }

    SDL_GameController *pad = NULL;
    if (SDL_NumJoysticks() > 0) pad = SDL_GameControllerOpen(0);

    SDL_Color white     = {255, 255, 255, 255};
    SDL_Color gray      = {150, 150, 162, 255};
    SDL_Color dimGray   = {110, 110, 122, 255};
    SDL_Color green     = {120, 220, 140, 255};
    SDL_Color red       = {230, 90, 90, 255};
    SDL_Color accent    = {0, 173, 181, 255};   // teal accent for selection/highlights
    SDL_Color headerBg  = {14, 14, 20, 255};
    SDL_Color tabBg     = {32, 32, 42, 255};
    SDL_Color rowBgAlt  = {26, 26, 34, 255};
    SDL_Color footerBg  = {14, 14, 20, 255};
    SDL_Color badgeB    = {235, 90, 90, 255};   // physical "B" button (download) - red-ish
    SDL_Color badgeA    = {235, 210, 90, 255};  // physical "A" button (exit) - yellow-ish
    SDL_Color badgeLR   = {90, 170, 235, 255};  // L/R badges - blue-ish

    // --- download and parse the json ---
    char *status_msg = NULL;
    bool loaded = fetch_and_parse_json();
    if (!loaded) {
        status_msg = "No se pudo descargar o leer el JSON. Revisa tu conexion y la URL.";
    }

    int cur_section = 0;
    int cur_item = 0;
    bool running = true;
    char footer[256] = "";

    // simple UI states: normal browsing, or showing a "are you sure?" confirmation dialog
    typedef enum { UI_MENU, UI_CONFIRM } UiState;
    UiState ui_state = UI_MENU;

    while (running && appletMainLoop()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;

            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                dlog("Boton pulsado, codigo SDL: %d  (ui_state=%d)", e.cbutton.button, ui_state);

                if (ui_state == UI_CONFIRM) {
                    // only two things matter while the confirmation dialog is shown:
                    // physical A (=SDL BUTTON_B) confirms, physical B (=SDL BUTTON_A) cancels.
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                        Item *it = &sections[cur_section].items[cur_item];
                        dlog("Confirmado, descargando item: %s", it->name);
                        dlog("  url: %s", it->url);
                        dlog("  dest: %s", it->dest);
                        dlog("  extract: %s", it->extract ? "true" : "false");

                        bool ok;
                        if (it->extract) {
                            const char *tmp_zip = "/switch-updater-tmp.zip";
                            ok = download_to_file(it->url, tmp_zip, renderer, fontBig);
                            dlog("download_to_file (zip temporal) devolvio: %s", ok ? "true" : "false");
                            if (ok) {
                                ok = extract_zip(tmp_zip, it->dest, it->strip, renderer, fontBig);
                                dlog("extract_zip devolvio: %s", ok ? "true" : "false");
                                remove(tmp_zip);
                            }
                        } else {
                            ok = download_to_file(it->url, it->dest, renderer, fontBig);
                            dlog("download_to_file devolvio: %s", ok ? "true" : "false");
                        }

                        snprintf(footer, sizeof(footer), ok ? "Listo: %s" : "Error: %s", it->name);
                        ui_state = UI_MENU;
                    } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                        dlog("Descarga cancelada por el usuario");
                        snprintf(footer, sizeof(footer), "Cancelado");
                        ui_state = UI_MENU;
                    }
                    // any other button is ignored while the dialog is open
                    continue;
                }

                // ---- normal menu navigation (ui_state == UI_MENU) ----
                switch (e.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                        if (loaded && sections[cur_section].item_count > 0)
                            cur_item = (cur_item + 1) % sections[cur_section].item_count;
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:
                        if (loaded && sections[cur_section].item_count > 0)
                            cur_item = (cur_item - 1 + sections[cur_section].item_count) % sections[cur_section].item_count;
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                        if (loaded && section_count > 0) {
                            cur_section = (cur_section + 1) % section_count;
                            cur_item = 0;
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                        if (loaded && section_count > 0) {
                            cur_section = (cur_section - 1 + section_count) % section_count;
                            cur_item = 0;
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_B:
                        // physical A button -> ask for confirmation instead of downloading immediately
                        if (loaded && sections[cur_section].item_count > 0) {
                            ui_state = UI_CONFIRM;
                            dlog("Mostrando confirmacion para: %s", sections[cur_section].items[cur_item].name);
                        }
                        break;
                    case SDL_CONTROLLER_BUTTON_A:
                        running = false;
                        break;
                    case SDL_CONTROLLER_BUTTON_START:
                        running = false;
                        break;
                    case SDL_CONTROLLER_BUTTON_X:
                        // refresh: re-download and re-parse the json without restarting the app.
                        // NOTE: physical button for this is whatever logs as "X" in the debug log;
                        // on Nintendo controllers X/Y are also typically swapped vs this SDL code name.
                        dlog("Refrescando JSON...");
                        snprintf(footer, sizeof(footer), "Actualizando lista...");
                        {
                            bool ok = fetch_and_parse_json();
                            loaded = ok;
                            if (ok) {
                                if (cur_section >= section_count) cur_section = 0;
                                if (section_count > 0 && cur_item >= sections[cur_section].item_count) cur_item = 0;
                                snprintf(footer, sizeof(footer), "Lista actualizada (%d secciones)", section_count);
                                status_msg = NULL;
                            } else {
                                status_msg = "No se pudo descargar o leer el JSON. Revisa tu conexion y la URL.";
                                snprintf(footer, sizeof(footer), "Error al actualizar");
                            }
                        }
                        break;
                }
            }
        }

        // ---- draw ----
        SDL_SetRenderDrawColor(renderer, 18, 18, 26, 255);
        SDL_RenderClear(renderer);

        // ----- header bar -----
        SDL_SetRenderDrawColor(renderer, headerBg.r, headerBg.g, headerBg.b, 255);
        SDL_Rect headerRect = {0, 0, SCREEN_W, 90};
        SDL_RenderFillRect(renderer, &headerRect);
        draw_text(renderer, fontBig, "Switch Updater", 60, 26, white);

        // refresh hint, top-right corner of the header (away from the action footer)
        const char *refreshHint = "X/Y: Refrescar lista";
        int rhw = text_width(fontSmall, refreshHint);
        draw_text(renderer, fontSmall, refreshHint, SCREEN_W - rhw - 40, 36, dimGray);

        // thin accent underline beneath the header
        SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, 255);
        SDL_Rect headerLine = {0, 88, SCREEN_W, 2};
        SDL_RenderFillRect(renderer, &headerLine);

        if (!loaded) {
            draw_text(renderer, font, status_msg ? status_msg : "Error", 60, 140, red);
        } else {
            // ----- section tabs (button-style, with item counts) -----
            int tabX = 50;
            int tabY = 116;
            int tabH = 52;
            for (int s = 0; s < section_count; s++) {
                char label[96];
                snprintf(label, sizeof(label), "%s (%d)", sections[s].name, sections[s].item_count);
                int tw = text_width(font, label);
                int tabW = tw + 48;
                bool selected = (s == cur_section);

                SDL_Rect tabRect = { tabX, tabY, tabW, tabH };
                if (selected) {
                    SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, 255);
                    SDL_RenderFillRect(renderer, &tabRect);
                } else {
                    SDL_SetRenderDrawColor(renderer, tabBg.r, tabBg.g, tabBg.b, 255);
                    SDL_RenderFillRect(renderer, &tabRect);
                    SDL_SetRenderDrawColor(renderer, dimGray.r, dimGray.g, dimGray.b, 255);
                    SDL_RenderDrawRect(renderer, &tabRect);
                }
                SDL_Color labelColor = selected ? headerBg : gray;
                draw_text_centered(renderer, font, label, tabX, tabW, tabY + 12, labelColor);

                tabX += tabW + 16;
            }

            // ----- item list for current section -----
            Section *sec = &sections[cur_section];
            int listTop = 196;
            int rowH = 56;
            int y = listTop;

            for (int i = 0; i < sec->item_count; i++) {
                bool selected = (i == cur_item);
                SDL_Rect rowRect = { 50, y, SCREEN_W - 100, rowH - 6 };

                if (selected) {
                    SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, 255);
                    SDL_RenderFillRect(renderer, &rowRect);
                } else if (i % 2 == 1) {
                    SDL_SetRenderDrawColor(renderer, rowBgAlt.r, rowBgAlt.g, rowBgAlt.b, 255);
                    SDL_RenderFillRect(renderer, &rowRect);
                }

                SDL_Color textColor = selected ? headerBg : white;

                // small square "icon" placeholder at the left of each row, tinted by section
                SDL_Rect iconRect = { rowRect.x + 14, y + (rowH - 6 - 20) / 2, 20, 20 };
                SDL_Color iconColor = selected ? headerBg : accent;
                SDL_SetRenderDrawColor(renderer, iconColor.r, iconColor.g, iconColor.b, 255);
                SDL_RenderFillRect(renderer, &iconRect);

                draw_text(renderer, font, sec->items[i].name, rowRect.x + 50, y + 10, textColor);

                y += rowH;
                if (y > SCREEN_H - 150) break;
            }

            if (sec->item_count == 0) {
                draw_text(renderer, font, "(seccion vacia)", 80, y, gray);
            }
        }

        // ----- footer bar -----
        SDL_SetRenderDrawColor(renderer, footerBg.r, footerBg.g, footerBg.b, 255);
        SDL_Rect footerRect = {0, SCREEN_H - 80, SCREEN_W, 80};
        SDL_RenderFillRect(renderer, &footerRect);
        SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, 255);
        SDL_Rect footerLine = {0, SCREEN_H - 80, SCREEN_W, 2};
        SDL_RenderFillRect(renderer, &footerLine);

        int fy = SCREEN_H - 56;
        int bx = 70;

        // NOTE: physical Switch A button fires SDL_CONTROLLER_BUTTON_B internally (download),
        // and physical B fires SDL_CONTROLLER_BUTTON_A (exit) -- labels here show the PHYSICAL letter.
        draw_button_badge(renderer, fontSmall, bx, fy, 16, "A", badgeA);
        draw_text(renderer, font, "Descargar", bx + 26, fy - 16, gray);
        bx += 26 + text_width(font, "Descargar") + 50;

        draw_button_badge(renderer, fontSmall, bx, fy, 16, "B", badgeB);
        draw_text(renderer, font, "Salir", bx + 26, fy - 16, gray);
        bx += 26 + text_width(font, "Salir") + 50;

        draw_button_badge(renderer, fontSmall, bx, fy, 16, "L", badgeLR);
        draw_button_badge(renderer, fontSmall, bx + 36, fy, 16, "R", badgeLR);
        draw_text(renderer, font, "Cambiar seccion", bx + 62, fy - 16, gray);

        // status footer message (download result / refresh status), its own row just above the footer bar
        if (footer[0] != 0) {
            int fw = text_width(font, footer);
            draw_text(renderer, font, footer, SCREEN_W - fw - 50, SCREEN_H - 112, green);
        }

        // ----- confirmation dialog overlay -----
        if (ui_state == UI_CONFIRM && loaded && sections[cur_section].item_count > 0) {
            Item *it = &sections[cur_section].items[cur_item];

            // dim the whole screen behind the dialog
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_Rect dim = {0, 0, SCREEN_W, SCREEN_H};
            SDL_RenderFillRect(renderer, &dim);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

            int boxW = 760, boxH = 240;
            int boxX = (SCREEN_W - boxW) / 2;
            int boxY = (SCREEN_H - boxH) / 2;
            SDL_Rect box = { boxX, boxY, boxW, boxH };

            SDL_SetRenderDrawColor(renderer, tabBg.r, tabBg.g, tabBg.b, 255);
            SDL_RenderFillRect(renderer, &box);
            SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, 255);
            SDL_RenderDrawRect(renderer, &box);

            draw_text_centered(renderer, font, "Confirmar descarga", boxX, boxW, boxY + 24, white);

            char itemLine[200];
            snprintf(itemLine, sizeof(itemLine), "%s", it->name);
            draw_text_centered(renderer, font, itemLine, boxX, boxW, boxY + 70, accent);

            const char *typeLine = it->extract ? "Se descargara y se extraera automaticamente" : "Se descargara el archivo";
            draw_text_centered(renderer, font, typeLine, boxX, boxW, boxY + 110, gray);

            // confirm buttons inside the dialog (physical labels, same convention as the footer)
            int cfy = boxY + boxH - 50;
            int cbx = boxX + 80;
            draw_button_badge(renderer, fontSmall, cbx, cfy, 16, "A", badgeA);
            draw_text(renderer, font, "Si, descargar", cbx + 26, cfy - 16, white);

            cbx = boxX + boxW - 240;
            draw_button_badge(renderer, fontSmall, cbx, cfy, 16, "B", badgeB);
            draw_text(renderer, font, "Cancelar", cbx + 26, cfy - 16, white);
        }

        SDL_RenderPresent(renderer);
    }

    if (pad) SDL_GameControllerClose(pad);
    TTF_CloseFont(font);
    TTF_CloseFont(fontBig);
    TTF_CloseFont(fontSmall);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    curl_global_cleanup();
    socketExit();
    romfsExit();
    log_close();
    return 0;
}
