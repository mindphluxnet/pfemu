/* Finding GOG's game.gog, on both hosts.
 *
 * GOG.com sells the Deluxe CD as DOSBox plus game.gog, a raw image of the
 * disc (docs/RELEASES.md, GOG section).  When that is installed and no
 * Deluxe is here yet, pfemu offers once to copy the game out of the image
 * into GOG/, beside the other installations, so nobody has to unpack it by
 * hand.  The GOG installation itself is only read.  What the copy is gets
 * decided by release_detect() like any other folder - the offer never makes
 * it a release by itself.  A "No" is remembered in pfemu-gog.cfg next to the
 * program (its own file, for the same reason as pfemu-winpos.cfg).
 *
 * This file decides whether to offer and where the image is.  Asking is the
 * host's: a MessageBox in the Windows launcher (src/launch.c), an SDL
 * message box on Linux (src/host_sdl.c).  Both say the same thing, from
 * gog_offer_text().
 *
 * The Windows and Linux editions ship the same image, byte for byte
 * (docs/RELEASES.md), so only the search differs:
 *
 *   Windows  the "path" value GOG's installer writes under
 *            HKLM\SOFTWARE\[WOW6432Node\]GOG.com\Games\1207664103, and
 *            game.gog directly in it.
 *   Linux    no registry.  GOG's .sh installer (MojoSetup) installs to
 *            <base>/GOG Games/Pinball Fantasies Deluxe, where base is ~,
 *            /opt or /usr/local/games, and puts the image in data/.
 *            Heroic records its installs in gog_store/installed.json.  Both
 *            layouts are tried in every candidate, because Heroic can just
 *            as well have installed the Windows edition under Wine.
 *   macOS    Heroic's installed.json under ~/Library/Application Support,
 *            which is where Heroic keeps its config there.  GOG Galaxy's
 *            own Mac install is not searched: its layout is not known yet.
 */
#include "compat.h"
#include "pfemu.h"

#define GOG_GAME_ID  "1207664103"
#define GOG_FILE     "pfemu-gog.cfg"

#ifdef _WIN32

/* <install path>\game.gog, from the key GOG's installer writes. */
int gog_find_image(char *out, size_t n){
    static const char *const keys[] = {
        "SOFTWARE\\WOW6432Node\\GOG.com\\Games\\" GOG_GAME_ID,
        "SOFTWARE\\GOG.com\\Games\\" GOG_GAME_ID,
    };
    char dir[MAX_PATH];
    int i;
    for(i = 0; i < 2; i++){
        DWORD len = sizeof(dir);
        if(RegGetValueA(HKEY_LOCAL_MACHINE, keys[i], "path", RRF_RT_REG_SZ,
                        NULL, dir, &len) != ERROR_SUCCESS) continue;
        snprintf(out, n, "%s\\game.gog", dir);
        if(GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

#else  /* ------------------------------------------------------- POSIX --- */

#define GOG_NAME "Pinball Fantasies Deluxe"

/* The image in one candidate directory: data/game.gog is the Linux
 * edition's layout, game.gog the Windows one. */
static int gog_try_dir(const char *dir, char *out, size_t n){
    static const char *const rel[] = { "data/game.gog", "game.gog" };
    struct stat st;
    int i;
    if(!dir || !*dir) return 0;
    for(i = 0; i < 2; i++){
        snprintf(out, n, "%s/%s", dir, rel[i]);
        if(stat(out, &st) == 0 && S_ISREG(st.st_mode)){
            fprintf(stderr, "[gog] found %s\n", out);
            return 1;
        }
    }
    return 0;
}

/* Heroic's installed.json: {"installed":[{..., "install_path":"...",
 * "appName":"1207664103", ...}, ...]}.  Not a JSON parser - the object
 * that holds our id is found by brace depth, and its install_path read as
 * a string with the usual escapes undone.  Anything that does not look
 * like that reads as "not installed here", which is also the answer for a
 * file that is not there at all. */
static int gog_heroic(const char *json, char *out, size_t n){
    FILE *f;
    char *buf, *p;
    long len;
    int found = 0;
    f = fopen(json, "rb");
    if(!f) return 0;
    if(fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 || len > (1L << 20)){
        fclose(f); return 0;
    }
    rewind(f);
    buf = (char*)malloc((size_t)len + 1);
    if(!buf){ fclose(f); return 0; }
    len = (long)fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[len] = 0;
    for(p = strstr(buf, "\"" GOG_GAME_ID "\""); p && !found;
        p = strstr(p + 1, "\"" GOG_GAME_ID "\"")){
        char *a = p, *b = p, *k, dir[1024];
        int depth = 0;
        size_t o = 0;
        /* Out to the enclosing object on both sides. */
        while(a > buf){
            a--;
            if(*a == '}') depth++;
            else if(*a == '{'){ if(!depth) break; depth--; }
        }
        depth = 0;
        while(*b){
            if(*b == '{') depth++;
            else if(*b == '}'){ if(!depth) break; depth--; }
            b++;
        }
        if(*a != '{' || *b != '}') continue;
        k = strstr(a, "\"install_path\"");
        if(!k || k > b) continue;
        k += strlen("\"install_path\"");
        while(k < b && (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n')) k++;
        if(k >= b || *k != ':') continue;
        k++;
        while(k < b && (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n')) k++;
        if(k >= b || *k != '"') continue;
        for(k++; k < b && *k != '"' && o + 1 < sizeof(dir); k++){
            if(*k == '\\' && k + 1 < b) k++;   /* \\ \" \/ */
            dir[o++] = *k;
        }
        dir[o] = 0;
        found = gog_try_dir(dir, out, n);
    }
    free(buf);
    return found;
}

int gog_find_image(char *out, size_t n){
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[1024];
    if(xdg && *xdg){
        snprintf(path, sizeof(path), "%s/heroic/gog_store/installed.json", xdg);
        if(gog_heroic(path, out, n)) return 1;
    }
    if(home && *home){
        static const char *const rel[] = {
            /* Heroic, native and Flatpak */
            ".config/heroic/gog_store/installed.json",
            ".var/app/com.heroicgameslauncher.hgl/config/heroic/gog_store/installed.json",
            /* Heroic on macOS */
            "Library/Application Support/heroic/gog_store/installed.json",
        };
        int i;
        for(i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); i++){
            snprintf(path, sizeof(path), "%s/%s", home, rel[i]);
            if(gog_heroic(path, out, n)) return 1;
        }
        /* GOG's own installer and Minigalaxy, then Heroic's default place
         * for when its json has moved. */
        snprintf(path, sizeof(path), "%s/GOG Games/" GOG_NAME, home);
        if(gog_try_dir(path, out, n)) return 1;
        snprintf(path, sizeof(path), "%s/Games/Heroic/" GOG_NAME, home);
        if(gog_try_dir(path, out, n)) return 1;
    }
    /* The other two places the installer offers. */
    if(gog_try_dir("/opt/GOG Games/" GOG_NAME, out, n)) return 1;
    if(gog_try_dir("/usr/local/games/GOG Games/" GOG_NAME, out, n)) return 1;
    return 0;
}

#endif /* _WIN32 */

/* 1, with the image path in image, when the import should be offered: no
 * runnable Deluxe among inst[0..n), no GOG folder yet, never declined, and
 * an image found. */
int gog_candidate(const RelResult *inst, int n, char *image, size_t len){
    char cfg[1080];
    int i;
    for(i = 0; i < n; i++)
        if(release_runnable(&inst[i]) && !_stricmp(inst[i].rel->id, "deluxe"))
            return 0;
    if(GetFileAttributesA(GOG_DIR) != INVALID_FILE_ATTRIBUTES) return 0;
    beside_exe(cfg, sizeof(cfg), GOG_FILE);
    if(GetFileAttributesA(cfg) != INVALID_FILE_ATTRIBUTES) return 0;
    return gog_find_image(image, len);
}

/* The question, the same on both hosts. */
void gog_offer_text(char *msg, size_t n, const char *image){
    snprintf(msg, n,
             "Pinball Fantasies Deluxe from GOG.com is installed here:\n\n"
             "    %s\n\n"
             "pfemu can copy the game out of its CD image into a folder named "
             GOG_DIR " beside your other installations (24 files, 3.7 MB). "
             "The GOG installation is not changed.\n\n"
             "Import it now? If you choose No, pfemu will not ask again.",
             image);
}

/* Remember a "No", so the offer is made once. */
void gog_decline(const char *image){
    char cfg[1080];
    FILE *f;
    beside_exe(cfg, sizeof(cfg), GOG_FILE);
    f = fopen(cfg, "w");
    if(!f) return;
    fprintf(f, "# The launcher offered to import the GOG version and"
               " was told No.\n# Delete this file to be asked again.\n");
    fprintf(f, "declined=%s\n", image);
    fclose(f);
}
