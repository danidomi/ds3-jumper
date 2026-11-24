/*
 * ds3-jumper — a tiny side-scrolling platformer for the PS3 DualShock 3.
 * C99 + SDL2. Runs on Linux, macOS and other Unix systems.
 */
#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#include "audio.h"

#define VW 960 /* logical screen size; scaled to the window */
#define VH 540
#define STEP (1.0f / 120.0f)

#define GRAV 2100.0f
#define JUMP_V 720.0f
#define WALK 230.0f
#define RUN 350.0f
#define PW 22.0f
#define PH 30.0f
#define EW 26.0f
#define EH 22.0f
#define DEATH_Y (VH + 120)

#define MAX_SOLIDS 256
#define MAX_COINS 512
#define MAX_ENEMIES 64
#define MAX_PARTS 512

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct { float x, y, w, h; bool ground; } Solid;
typedef struct { float x, y; } Coin;
typedef enum { EN_WALKER, EN_HOPPER, EN_FLYER, EN_SPIKY, EN_ARMORED, EN_KINDS } EnemyKind;
typedef struct {
    EnemyKind kind;
    float x, y, vx, vy, minx, maxx;
    float prevX, prevY; /* position at the previous physics step (for smooth drawing) */
    float baseY;  /* ground line (hopper) or flight centre (flyer) */
    float t;      /* per-enemy timer: hop countdown / flight phase */
    int hp;       /* armored: 2 = helmet on */
} Enemy;

typedef enum { DIFF_EASY, DIFF_MEDIUM, DIFF_HARD, DIFF_COUNT } Difficulty;
typedef struct {
    const char *name;
    int lives;
    float enemyChance, enemySpeed, rampDist, unlockScale, gapScale, invuln;
} DiffParams;
/* rampDist: distance (px) until the world is fully hard.
 * unlockScale: multiplies the distance at which each enemy type first appears. */
static const DiffParams DIFFS[DIFF_COUNT] = {
    { "EASY",   5, 0.65f, 0.75f, 40000, 1.6f, 0.80f, 2.0f },
    { "MEDIUM", 3, 1.00f, 1.00f, 25000, 1.0f, 1.00f, 1.5f },
    { "HARD",   2, 1.40f, 1.30f, 14000, 0.4f, 1.15f, 1.0f },
};
/* Distance (px, MEDIUM) where each enemy type starts to appear. */
static const float UNLOCK_AT[EN_KINDS] = { 0, 2500, 6000, 9000, 13000 };
static const int STOMP_POINTS[EN_KINDS] = { 50, 75, 100, 0, 150 };
typedef struct { float x, y, vx, vy, life, max, size; Uint8 r, g, b; } Part;

typedef enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER, ST_SETTINGS } State;

typedef struct {
    bool jump, run, start, select, tri, ps, confirm, back;
    bool up, down, left, right;
} Held;

typedef struct {
    float move;
    bool jumpHeld, runHeld;
    bool jumpP, startP, selectP, triP, psP, confirmP, backP;
    bool upP, downP, leftP, rightP; /* with auto-repeat, for menus */
} Input;

/* ------------------------------------------------------------- palettes */

typedef struct { const char *name; Uint8 r, g, b; } Swatch;

static const Swatch HAIR[] = {
    { "BROWN", 90, 55, 30 }, { "BLACK", 30, 30, 36 }, { "BLONDE", 240, 205, 95 },
    { "RED", 200, 70, 35 }, { "WHITE", 235, 235, 240 }, { "BLUE", 60, 115, 235 },
    { "PINK", 245, 115, 185 }, { "GREEN", 70, 185, 95 }, { "PURPLE", 140, 80, 200 },
};
static const Swatch SKIN[] = {
    { "LIGHT", 250, 214, 176 }, { "PALE", 255, 230, 210 }, { "TAN", 222, 172, 122 },
    { "BROWN", 172, 116, 76 }, { "DARK", 112, 72, 46 },
};
static const Swatch SHIRT[] = {
    { "RED", 220, 60, 60 }, { "BLUE", 60, 110, 220 }, { "GREEN", 60, 170, 80 },
    { "PURPLE", 140, 70, 190 }, { "ORANGE", 240, 140, 40 }, { "TEAL", 40, 170, 170 },
    { "BLACK", 45, 45, 55 }, { "WHITE", 235, 235, 240 },
};
static const Swatch PANTS[] = {
    { "BLUE", 40, 60, 120 }, { "BLACK", 30, 30, 36 }, { "BROWN", 100, 70, 45 },
    { "GREY", 110, 115, 125 }, { "GREEN", 50, 100, 60 }, { "RED", 140, 40, 40 },
};
static const Swatch SCARF[] = {
    { "YELLOW", 255, 205, 60 }, { "RED", 230, 50, 60 }, { "BLUE", 70, 140, 255 },
    { "GREEN", 80, 210, 100 }, { "PINK", 255, 130, 200 }, { "WHITE", 245, 245, 250 },
    { "NONE", 0, 0, 0 },
};

/* ------------------------------------------------------------- settings */

static struct {
    int hair, skin, shirt, pants, scarf;
    bool sfx, music, rumble;
    int volume; /* 0..10 */
    int fps;    /* index into FPS_OPTIONS */
    int difficulty;
} CFG = { 0, 0, 0, 0, 0, true, true, true, 7, 1, DIFF_MEDIUM };

#define DP (DIFFS[CFG.difficulty])

static const int FPS_OPTIONS[] = { 30, 60, 90, 120 };

static char savePath[1024], settingsPath[1024];

/* -------------------------------------------------------------- game state */

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_GameController *pad;
    bool fullscreen, quit;
    State st, settingsReturn;
    int menuSel, settingsSel;

    float px, py, vx, vy;
    bool onGround;
    float coyote, jumpBuf, invuln, squash, runAnim;
    int facing;
    float safeX, safeY;

    float prevPx, prevPy, prevCamX; /* previous physics step, for interpolation */
    float scale, offX, offY;        /* logical -> device pixels */
    int lives, maxLives, coins, bonus, score, best, stomps;
    Difficulty runDiff; /* difficulty of the current run */
    bool runActive, noSave, newBest, playedThisSession, sessionNewBest;

    /* dev mode */
    bool dev;
    int stepsThisFrame;
    float fpsTimer, fps, frameMs, frameMsMax, frameMsMaxAcc;
    int fpsFrames;
    float updMs, drawMs, updAcc, drawAcc, cpuPct, memMB;
    double lastCpuSec;
    float runSeconds;
    float dist, camX, time, shake;

    Solid solids[MAX_SOLIDS]; int nSolids;
    Coin coinArr[MAX_COINS]; int nCoins;
    Enemy enemies[MAX_ENEMIES]; int nEnemies;
    Part parts[MAX_PARTS]; int nParts;

    float genX, genY;
    Uint32 rng;
    Held prev;
    float repeat[4];
} G;

/* ------------------------------------------------------------------ utils */

static float frand(void) {
    G.rng ^= G.rng << 13; G.rng ^= G.rng >> 17; G.rng ^= G.rng << 5;
    return (float)(G.rng & 0xFFFFFF) / 16777216.0f;
}

static float hashf(int i) {
    Uint32 x = (Uint32)i * 2654435761u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return (float)(x & 0xFFFF) / 65535.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool overlap(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static void rumble(Uint16 lo, Uint16 hi, Uint32 ms) {
    if (G.pad && CFG.rumble) SDL_GameControllerRumble(G.pad, lo, hi, ms);
}

static void applyAudioConfig(void) { audio_config(CFG.sfx, CFG.music, CFG.volume); }

/* ------------------------------------------------------- settings file */

static int findSwatch(const Swatch *list, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].name, name)) return i;
    return 0;
}

static void loadSettings(void) {
    FILE *f = settingsPath[0] ? fopen(settingsPath, "r") : NULL;
    if (!f) return;
    char line[256], key[64], val[64];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
        for (char *c = val; *c; c++) *c = (char)toupper((unsigned char)*c);
        bool on = !strcmp(val, "ON") || !strcmp(val, "TRUE") || !strcmp(val, "1");
        if (!strcmp(key, "hair")) CFG.hair = findSwatch(HAIR, LEN(HAIR), val);
        else if (!strcmp(key, "skin")) CFG.skin = findSwatch(SKIN, LEN(SKIN), val);
        else if (!strcmp(key, "shirt")) CFG.shirt = findSwatch(SHIRT, LEN(SHIRT), val);
        else if (!strcmp(key, "pants")) CFG.pants = findSwatch(PANTS, LEN(PANTS), val);
        else if (!strcmp(key, "scarf")) CFG.scarf = findSwatch(SCARF, LEN(SCARF), val);
        else if (!strcmp(key, "sound_effects")) CFG.sfx = on;
        else if (!strcmp(key, "music")) CFG.music = on;
        else if (!strcmp(key, "rumble")) CFG.rumble = on;
        else if (!strcmp(key, "volume")) CFG.volume = (int)clampf((float)atoi(val), 0, 10);
        else if (!strcmp(key, "difficulty")) {
            for (int i = 0; i < DIFF_COUNT; i++)
                if (!strcmp(DIFFS[i].name, val)) CFG.difficulty = i;
        } else if (!strcmp(key, "fps")) {
            for (int i = 0; i < (int)LEN(FPS_OPTIONS); i++)
                if (FPS_OPTIONS[i] == atoi(val)) CFG.fps = i;
        }
    }
    fclose(f);
}

static void writeSettings(void) {
    if (G.noSave || !settingsPath[0]) return;
    FILE *f = fopen(settingsPath, "w");
    if (!f) { fprintf(stderr, "could not write settings file %s\n", settingsPath); return; }
    fprintf(f, "# ds3-jumper settings (also editable in the game: Settings menu)\n");
    fprintf(f, "hair = %s\n", HAIR[CFG.hair].name);
    fprintf(f, "skin = %s\n", SKIN[CFG.skin].name);
    fprintf(f, "shirt = %s\n", SHIRT[CFG.shirt].name);
    fprintf(f, "pants = %s\n", PANTS[CFG.pants].name);
    fprintf(f, "scarf = %s\n", SCARF[CFG.scarf].name);
    fprintf(f, "sound_effects = %s\n", CFG.sfx ? "on" : "off");
    fprintf(f, "music = %s\n", CFG.music ? "on" : "off");
    fprintf(f, "volume = %d\n", CFG.volume);
    fprintf(f, "rumble = %s\n", CFG.rumble ? "on" : "off");
    fprintf(f, "fps = %d\n", FPS_OPTIONS[CFG.fps]);
    fprintf(f, "difficulty = %s\n", DIFFS[CFG.difficulty].name);
    fclose(f);
}

/* ------------------------------------------------------------ save file */

/* Plain "key=value" text file in the per-user data directory. */
static struct {
    int best, last, games, coinsTotal, bestCoins, stompsTotal;
    int bestD[DIFF_COUNT]; /* best score per difficulty */
    double distanceTotal, playSeconds;
} S;

static void initPaths(void) {
    char *dir = SDL_GetPrefPath("danidomi", "ds3-jumper"); /* creates the folder */
    const char *env = getenv("DS3_JUMPER_SAVE");
    if (env && *env) snprintf(savePath, sizeof savePath, "%s", env);
    else if (dir) snprintf(savePath, sizeof savePath, "%ssave.txt", dir);
    env = getenv("DS3_JUMPER_SETTINGS");
    if (env && *env) snprintf(settingsPath, sizeof settingsPath, "%s", env);
    else if (dir) snprintf(settingsPath, sizeof settingsPath, "%ssettings.txt", dir);
    if (dir) SDL_free(dir);
}

static void loadSave(void) {
    FILE *f = savePath[0] ? fopen(savePath, "r") : NULL;
    if (!f) return;
    char line[256], key[64];
    double v;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %lf", key, &v) != 2) continue;
        if (!strcmp(key, "best_score")) S.best = (int)v;
        else if (!strcmp(key, "best_score_easy")) S.bestD[DIFF_EASY] = (int)v;
        else if (!strcmp(key, "best_score_medium")) S.bestD[DIFF_MEDIUM] = (int)v;
        else if (!strcmp(key, "best_score_hard")) S.bestD[DIFF_HARD] = (int)v;
        else if (!strcmp(key, "last_score")) S.last = (int)v;
        else if (!strcmp(key, "games_played")) S.games = (int)v;
        else if (!strcmp(key, "coins_total")) S.coinsTotal = (int)v;
        else if (!strcmp(key, "best_coins")) S.bestCoins = (int)v;
        else if (!strcmp(key, "enemies_stomped")) S.stompsTotal = (int)v;
        else if (!strcmp(key, "distance_total")) S.distanceTotal = v;
        else if (!strcmp(key, "play_seconds")) S.playSeconds = v;
    }
    fclose(f);
    /* Saves from before difficulties existed were played on MEDIUM */
    if (!S.bestD[DIFF_EASY] && !S.bestD[DIFF_MEDIUM] && !S.bestD[DIFF_HARD]) S.bestD[DIFF_MEDIUM] = S.best;
}

static void writeSave(void) {
    if (G.noSave || !savePath[0]) return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", savePath);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "could not write save file %s\n", savePath); return; }
    char date[32] = "";
    time_t now = time(NULL);
    strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(f, "# ds3-jumper save file\n");
    fprintf(f, "best_score = %d\n", S.best);
    fprintf(f, "best_score_easy = %d\n", S.bestD[DIFF_EASY]);
    fprintf(f, "best_score_medium = %d\n", S.bestD[DIFF_MEDIUM]);
    fprintf(f, "best_score_hard = %d\n", S.bestD[DIFF_HARD]);
    fprintf(f, "last_score = %d\n", S.last);
    fprintf(f, "games_played = %d\n", S.games);
    fprintf(f, "coins_total = %d\n", S.coinsTotal);
    fprintf(f, "best_coins = %d\n", S.bestCoins);
    fprintf(f, "enemies_stomped = %d\n", S.stompsTotal);
    fprintf(f, "distance_total = %.0f\n", S.distanceTotal);
    fprintf(f, "play_seconds = %.0f\n", S.playSeconds);
    fprintf(f, "last_played = %s\n", date);
    fclose(f);
    rename(tmp, savePath); /* atomic replace */
}

/* Record the current run (game over, restart or quit) and save. */
static void endRun(void) {
    if (!G.runActive) return;
    G.runActive = false;
    if (G.score <= 0 && G.runSeconds < 1) return; /* never really played */
    S.games++;
    S.last = G.score;
    G.playedThisSession = true;
    G.newBest = G.score > S.bestD[G.runDiff];
    if (G.newBest) { S.bestD[G.runDiff] = G.score; G.sessionNewBest = true; }
    if (G.score > S.best) S.best = G.score;
    S.coinsTotal += G.coins;
    if (G.coins > S.bestCoins) S.bestCoins = G.coins;
    S.stompsTotal += G.stomps;
    S.distanceTotal += G.dist;
    S.playSeconds += G.runSeconds;
    G.best = S.bestD[G.runDiff];
    writeSave();
}

/* -------------------------------------------------------------- particles */

static void spawn(float x, float y, int n, Uint8 r, Uint8 g, Uint8 b, float speed, float up) {
    for (int i = 0; i < n && G.nParts < MAX_PARTS; i++) {
        Part *p = &G.parts[G.nParts++];
        float a = frand() * 6.2832f;
        float s = speed * (0.3f + frand() * 0.7f);
        p->x = x; p->y = y;
        p->vx = cosf(a) * s;
        p->vy = sinf(a) * s - up;
        p->max = p->life = 0.35f + frand() * 0.4f;
        p->size = 2 + frand() * 3;
        p->r = r; p->g = g; p->b = b;
    }
}

static void updateParticles(float dt) {
    for (int i = 0; i < G.nParts;) {
        Part *p = &G.parts[i];
        p->life -= dt;
        if (p->life <= 0) { G.parts[i] = G.parts[--G.nParts]; continue; }
        p->vy += 900 * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        i++;
    }
}

/* ------------------------------------------------------------ level gen */

static void addSolid(float x, float y, float w, float h, bool ground) {
    if (G.nSolids >= MAX_SOLIDS) return;
    G.solids[G.nSolids++] = (Solid){ x, y, w, h, ground };
}

static void addCoin(float x, float y) {
    if (G.nCoins >= MAX_COINS) return;
    G.coinArr[G.nCoins++] = (Coin){ x, y };
}

static void enemySize(EnemyKind k, float *w, float *h) {
    switch (k) {
    case EN_HOPPER: *w = 24; *h = 20; break;
    case EN_FLYER: *w = 26; *h = 18; break;
    case EN_SPIKY: *w = 28; *h = 22; break;
    case EN_ARMORED: *w = 30; *h = 24; break;
    default: *w = EW; *h = EH; break;
    }
}

static float enemyBaseSpeed(EnemyKind k) {
    switch (k) {
    case EN_HOPPER: return 45;
    case EN_FLYER: return 80;
    case EN_SPIKY: return 40;
    case EN_ARMORED: return 50;
    default: return 60;
    }
}

/* Pick an enemy type among those unlocked at this distance; newer ones are rarer. */
static EnemyKind pickEnemy(float x) {
    float weights[EN_KINDS], total = 0;
    for (int k = 0; k < EN_KINDS; k++) {
        float at = UNLOCK_AT[k] * DP.unlockScale;
        weights[k] = x < at ? 0 : k == EN_WALKER ? 3.0f : clampf((x - at) / 4000.0f, 0.4f, 1.6f);
        total += weights[k];
    }
    float r = frand() * total;
    for (int k = 0; k < EN_KINDS; k++) {
        if (r < weights[k]) return (EnemyKind)k;
        r -= weights[k];
    }
    return EN_WALKER;
}

/* groundY = top of the ground the enemy lives on. */
static void addEnemy(EnemyKind k, float minx, float maxx, float groundY) {
    float w, h;
    enemySize(k, &w, &h);
    maxx -= w;
    if (G.nEnemies >= MAX_ENEMIES || maxx <= minx) return;
    Enemy e = { 0 };
    e.kind = k;
    e.minx = minx; e.maxx = maxx;
    e.x = minx + frand() * (maxx - minx);
    e.y = groundY - h;
    e.vx = enemyBaseSpeed(k) * DP.enemySpeed * (frand() < 0.5f ? -1 : 1);
    e.baseY = e.y;
    e.t = 0.6f + frand();
    e.hp = k == EN_ARMORED ? 2 : 1;
    if (k == EN_FLYER) { e.baseY = groundY - 95 - frand() * 25; e.y = e.baseY; e.t = frand() * 6.28f; }
    e.prevX = e.x; e.prevY = e.y;
    G.enemies[G.nEnemies++] = e;
}

/* Time in the air for a full jump that lands `rise` px higher (negative = lower). */
static float airTime(float rise) {
    float d = JUMP_V * JUMP_V - 2 * GRAV * rise;
    if (d < 0) return 0;
    return (JUMP_V + sqrtf(d)) / GRAV;
}

static void genChunk(void) {
    float diff = clampf(G.genX / DP.rampDist, 0, 1);

    float dy = (frand() - 0.5f) * 2 * (40 + 40 * diff);
    float newY = clampf(G.genY + dy, 300, 470);
    float rise = G.genY - newY;
    if (rise > 80) { newY = G.genY - 80; rise = 80; }

    float gap = (40 + frand() * (60 + 120 * diff)) * DP.gapScale;
    float maxGap = 0.75f * RUN * airTime(rise) - PW;
    if (gap > maxGap) gap = maxGap;
    if (frand() < 0.12f) gap = 0;

    /* A small stepping platform over wide gaps */
    if (gap > 150 && frand() < 0.6f) {
        float top = fminf(G.genY, newY) - 50;
        addSolid(G.genX + gap / 2 - 35, top, 70, 14, false);
    }

    float x = G.genX + gap;
    float w = 240 + frand() * (380 - 160 * diff);
    addSolid(x, newY, w, VH + 300 - newY, true);

    if (frand() < 0.55f) {
        int n = 3 + (int)(frand() * 3);
        float cx = x + 40 + frand() * fmaxf(1, w - 80 - n * 26);
        for (int i = 0; i < n; i++) addCoin(cx + i * 26, newY - 40);
    }

    if (w > 280 && frand() < (0.3f + 0.45f * diff) * DP.enemyChance) {
        EnemyKind k = pickEnemy(x);
        if (k == EN_FLYER) addEnemy(k, x - gap * 0.5f, x + w, newY); /* may cross the gap */
        else addEnemy(k, x + 20, x + w - 20, newY);
        /* long platforms can get a second enemy further on */
        if (w > 420 && frand() < 0.35f * diff * DP.enemyChance)
            addEnemy(pickEnemy(x), x + w * 0.55f, x + w - 20, newY);
    }

    if (frand() < 0.45f) {
        float fw = 80 + frand() * 90;
        float fx = x + frand() * fmaxf(1, w - fw);
        float fy = newY - 90 - frand() * 25; /* a full jump rises ~120 px */
        addSolid(fx, fy, fw, 14, false);
        int n = (int)(fw / 28);
        for (int i = 0; i < n; i++) addCoin(fx + 14 + i * 28, fy - 26);
    }

    G.genX = x + w;
    G.genY = newY;
}

static void prune(void) {
    float edge = G.camX - 500;
    int j = 0;
    for (int i = 0; i < G.nSolids; i++)
        if (G.solids[i].x + G.solids[i].w > edge) G.solids[j++] = G.solids[i];
    G.nSolids = j;
    j = 0;
    for (int i = 0; i < G.nCoins; i++)
        if (G.coinArr[i].x > edge) G.coinArr[j++] = G.coinArr[i];
    G.nCoins = j;
    j = 0;
    for (int i = 0; i < G.nEnemies; i++)
        if (G.enemies[i].maxx + 40 > edge) G.enemies[j++] = G.enemies[i];
    G.nEnemies = j;
}

/* --------------------------------------------------------------- game */

static void resetWorld(void) {
    G.nSolids = G.nCoins = G.nEnemies = G.nParts = 0;
    G.rng = (SDL_GetTicks() * 2654435761u) | 1u;
    addSolid(-300, 420, 1300, VH + 300 - 420, true);
    G.genX = 1000; G.genY = 420;
    G.px = 120; G.py = 420 - PH; G.vx = G.vy = 0;
    G.onGround = true; G.facing = 1;
    G.coyote = G.jumpBuf = G.invuln = G.squash = 0;
    G.safeX = G.px; G.safeY = G.py;
    G.camX = 0; G.dist = 0;
    G.prevPx = G.px; G.prevPy = G.py; G.prevCamX = 0;
    G.runDiff = (Difficulty)CFG.difficulty;
    G.maxLives = G.lives = DP.lives;
    G.best = S.bestD[G.runDiff];
    G.coins = G.bonus = G.score = G.stomps = 0;
    G.shake = 0;
    while (G.genX < G.camX + VW + 400) genChunk();
}

static void newGame(void) {
    endRun();
    resetWorld();
    G.runActive = true; G.runSeconds = 0;
    G.st = ST_PLAY;
    if (G.pad) SDL_GameControllerSetPlayerIndex(G.pad, 0);
    audio_play(SFX_START);
    audio_music(true);
}

static void toTitle(void) {
    endRun();
    resetWorld();
    G.st = ST_TITLE;
    G.menuSel = 0;
    audio_music(true);
}

static void gameOver(void) {
    G.st = ST_OVER;
    endRun();
    audio_music(false);
    audio_play(SFX_GAMEOVER);
}

static void hurt(bool fall) {
    G.lives--;
    G.shake = 0.3f;
    rumble(0xFFFF, 0xFFFF, 350);
    audio_play(fall ? SFX_FALL : SFX_HURT);
    spawn(G.px + PW / 2, G.py + PH / 2, 24, 230, 70, 70, 260, 120);
    if (G.lives <= 0) { gameOver(); return; }
    G.invuln = DIFFS[G.runDiff].invuln;
}

static void fell(void) {
    hurt(true);
    if (G.st != ST_PLAY) return;
    G.px = G.safeX; G.py = G.safeY - 40;
    G.vx = G.vy = 0;
    if (G.camX > G.px - 120) G.camX = G.px - 120;
    G.prevPx = G.px; G.prevPy = G.py; G.prevCamX = G.camX;
    for (int i = 0; i < G.nEnemies; i++)
        if (fabsf(G.enemies[i].x - G.px) < 180) G.enemies[i] = G.enemies[--G.nEnemies], i--;
}

static void update(float dt, const Input *in) {
    G.prevPx = G.px; G.prevPy = G.py; G.prevCamX = G.camX;
    for (int i = 0; i < G.nEnemies; i++) { G.enemies[i].prevX = G.enemies[i].x; G.enemies[i].prevY = G.enemies[i].y; }
    /* Horizontal movement */
    float target = in->move * (in->runHeld ? RUN : WALK);
    float accel = G.onGround ? (in->move == 0 ? 2600.0f : 2200.0f) : 1300.0f;
    if (G.vx < target) G.vx = fminf(target, G.vx + accel * dt);
    else if (G.vx > target) G.vx = fmaxf(target, G.vx - accel * dt);
    if (in->move > 0.1f) G.facing = 1;
    else if (in->move < -0.1f) G.facing = -1;

    /* Jump: coyote time + input buffer + variable height */
    G.coyote = G.onGround ? 0.1f : G.coyote - dt;
    G.jumpBuf -= dt;
    if (G.jumpBuf > 0 && G.coyote > 0) {
        G.vy = -JUMP_V;
        G.jumpBuf = G.coyote = 0;
        G.onGround = false;
        audio_play(SFX_JUMP);
        spawn(G.px + PW / 2, G.py + PH, 6, 230, 230, 220, 90, 0);
    }
    if (!in->jumpHeld && G.vy < -260) G.vy = -260;
    G.vy = fminf(G.vy + GRAV * dt, 1100);

    /* Move X and resolve against ground blocks (platforms are one-way) */
    G.px += G.vx * dt;
    if (G.px < G.camX) { G.px = G.camX; if (G.vx < 0) G.vx = 0; }
    for (int i = 0; i < G.nSolids; i++) {
        Solid *s = &G.solids[i];
        if (!s->ground || !overlap(G.px, G.py, PW, PH, s->x, s->y, s->w, s->h)) continue;
        if (G.px + PW / 2 < s->x + s->w / 2) G.px = s->x - PW; else G.px = s->x + s->w;
        G.vx = 0;
    }

    /* Move Y */
    float prevBottom = G.py + PH;
    bool wasGround = G.onGround;
    float fallSpeed = G.vy;
    G.py += G.vy * dt;
    G.onGround = false;
    for (int i = 0; i < G.nSolids; i++) {
        Solid *s = &G.solids[i];
        if (!overlap(G.px, G.py, PW, PH, s->x, s->y, s->w, s->h)) continue;
        if (!s->ground) {
            if (G.vy >= 0 && prevBottom <= s->y + 1) { G.py = s->y - PH; G.vy = 0; G.onGround = true; }
            continue;
        }
        if (G.vy > 0) { G.py = s->y - PH; G.vy = 0; G.onGround = true; }
        else if (G.vy < 0) { G.py = s->y + s->h; G.vy = 0; }
    }
    if (G.onGround && !wasGround) {
        G.squash = 1;
        if (fallSpeed > 300) audio_play(SFX_LAND);
        spawn(G.px + PW / 2, G.py + PH, 8, 200, 190, 170, 110, 40);
    }
    if (G.onGround) { G.safeX = G.px; G.safeY = G.py; }

    G.runAnim += fabsf(G.vx) * dt * 0.06f;
    G.squash = fmaxf(0, G.squash - dt * 6);
    G.invuln = fmaxf(0, G.invuln - dt);

    if (G.py > DEATH_Y) { fell(); return; }

    /* Enemies */
    for (int i = 0; i < G.nEnemies; i++) {
        Enemy *e = &G.enemies[i];
        if (e->x > G.camX + VW + 100) continue;
        float ew, eh;
        enemySize(e->kind, &ew, &eh);
        e->x += e->vx * dt;
        if (e->x < e->minx) { e->x = e->minx; e->vx = fabsf(e->vx); }
        if (e->x > e->maxx) { e->x = e->maxx; e->vx = -fabsf(e->vx); }
        if (e->kind == EN_HOPPER) {
            e->t -= dt;
            if (e->y >= e->baseY && e->t <= 0) { e->vy = -560; e->t = 1.0f + frand() * 0.8f; }
            e->vy += GRAV * dt;
            e->y += e->vy * dt;
            if (e->y >= e->baseY) { e->y = e->baseY; e->vy = 0; }
        } else if (e->kind == EN_FLYER) {
            e->t += dt * 2.6f;
            e->y = e->baseY + sinf(e->t) * 32;
        }
        if (!overlap(G.px, G.py, PW, PH, e->x, e->y, ew, eh)) continue;
        bool fromAbove = G.vy > 0 && G.py + PH - e->y < 16;
        if (fromAbove && e->kind != EN_SPIKY) {
            G.vy = in->jumpHeld ? -620 : -420;
            audio_play(SFX_STOMP);
            rumble(0x6000, 0, 90);
            if (e->kind == EN_ARMORED && e->hp > 1) { /* helmet flies off, now it's angry */
                e->hp--;
                e->vx = (e->vx < 0 ? -1 : 1) * 120 * DIFFS[G.runDiff].enemySpeed;
                spawn(e->x + ew / 2, e->y, 12, 150, 160, 175, 200, 120);
                G.bonus += 25;
                continue;
            }
            spawn(e->x + ew / 2, e->y + eh / 2, 18, 160, 80, 190, 220, 80);
            G.bonus += STOMP_POINTS[e->kind];
            G.stomps++;
            G.enemies[i] = G.enemies[--G.nEnemies]; i--;
        } else if (G.invuln <= 0) {
            G.vy = -380;
            G.vx = (G.px < e->x) ? -250 : 250;
            hurt(false);
            if (G.st != ST_PLAY) return;
        } else if (fromAbove) {
            G.vy = -300; /* bounce off spikes while invulnerable */
        }
    }

    /* Coins */
    for (int i = 0; i < G.nCoins; i++) {
        Coin *c = &G.coinArr[i];
        if (!overlap(G.px, G.py, PW, PH, c->x - 7, c->y - 7, 14, 14)) continue;
        spawn(c->x, c->y, 8, 255, 220, 70, 120, 60);
        G.coinArr[i] = G.coinArr[--G.nCoins]; i--;
        G.coins++;
        audio_play(SFX_COIN);
        rumble(0, 0x8000, 40);
    }

    /* Camera, world generation, score */
    float tgt = G.px - VW * 0.35f;
    if (tgt > G.camX) G.camX += (tgt - G.camX) * fminf(1, dt * 10);
    while (G.genX < G.camX + VW + 400) genChunk();
    prune();

    G.runSeconds += dt;
    G.dist = fmaxf(G.dist, G.px / 10);
    G.score = (int)G.dist + G.coins * 10 + G.bonus;
}

/* --------------------------------------------------------------- input */

static bool btn(SDL_GameControllerButton b) {
    return G.pad && SDL_GameControllerGetButton(G.pad, b);
}

static float axis(SDL_GameControllerAxis a) {
    return G.pad ? SDL_GameControllerGetAxis(G.pad, a) / 32767.0f : 0;
}

/* Edge + auto-repeat for menu navigation. */
static bool navPress(bool held, bool wasHeld, int i, float dt) {
    if (!held) { G.repeat[i] = 0; return false; }
    if (!wasHeld) { G.repeat[i] = 0.35f; return true; }
    G.repeat[i] -= dt;
    if (G.repeat[i] <= 0) { G.repeat[i] = 0.11f; return true; }
    return false;
}

static void readInput(Input *in, float dt) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    Held h = { 0 };
    float mv = 0;

    h.left = k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A];
    h.right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
    h.up = k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W];
    h.down = k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S];
    h.jump = k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W] || k[SDL_SCANCODE_Z];
    h.run = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_X];
    h.start = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_P];
    h.select = k[SDL_SCANCODE_F3] || k[SDL_SCANCODE_GRAVE]; /* dev mode */
    h.tri = k[SDL_SCANCODE_F] || k[SDL_SCANCODE_F11];
    h.ps = k[SDL_SCANCODE_Q];
    h.confirm = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE];
    h.back = k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE];

    if (G.pad) {
        float ax = axis(SDL_CONTROLLER_AXIS_LEFTX), ay = axis(SDL_CONTROLLER_AXIS_LEFTY);
        if (fabsf(ax) > 0.2f) mv += (ax - copysignf(0.2f, ax)) / 0.8f;
        h.left |= btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -0.5f;
        h.right |= btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 0.5f;
        h.up |= btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -0.5f;
        h.down |= btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 0.5f;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mv -= 1;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mv += 1;
        h.jump |= btn(SDL_CONTROLLER_BUTTON_A);          /* Cross    */
        h.confirm |= btn(SDL_CONTROLLER_BUTTON_A);
        h.back |= btn(SDL_CONTROLLER_BUTTON_B);          /* Circle   */
        h.run |= btn(SDL_CONTROLLER_BUTTON_X)            /* Square   */
              || axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.3f;
        h.start |= btn(SDL_CONTROLLER_BUTTON_START);
        h.select |= btn(SDL_CONTROLLER_BUTTON_BACK);
        h.tri |= btn(SDL_CONTROLLER_BUTTON_Y);           /* Triangle */
        h.ps |= btn(SDL_CONTROLLER_BUTTON_GUIDE);        /* PS       */
    }
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) mv -= 1;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) mv += 1;

    in->move = clampf(mv, -1, 1);
    in->jumpHeld = h.jump;
    in->runHeld = h.run;
    in->jumpP = h.jump && !G.prev.jump;
    in->startP = h.start && !G.prev.start;
    in->selectP = h.select && !G.prev.select;
    in->triP = h.tri && !G.prev.tri;
    in->psP = h.ps && !G.prev.ps;
    in->confirmP = h.confirm && !G.prev.confirm;
    in->backP = h.back && !G.prev.back;
    in->upP = navPress(h.up, G.prev.up, 0, dt);
    in->downP = navPress(h.down, G.prev.down, 1, dt);
    in->leftP = navPress(h.left, G.prev.left, 2, dt);
    in->rightP = navPress(h.right, G.prev.right, 3, dt);
    G.prev = h;
}

static void openPad(int index) {
    if (G.pad || !SDL_IsGameController(index)) return;
    G.pad = SDL_GameControllerOpen(index);
    if (G.pad) {
        SDL_GameControllerSetPlayerIndex(G.pad, 0); /* LED 1 on, stops the blinking */
        rumble(0x4000, 0x4000, 150);
    }
}

/* -------------------------------------------------------------- drawing */

static float shakeX, shakeY;

static void color(Uint8 r, Uint8 g, Uint8 b, Uint8 a) { SDL_SetRenderDrawColor(G.ren, r, g, b, a); }
static void colorS(const Swatch *s) { color(s->r, s->g, s->b, 255); }

/* Logical (960x540) rect -> device pixels. Both edges are rounded, so a
 * rect keeps the same pixel size wherever it is on screen (no shimmering
 * when the scale isn't a whole number). */
static void fillS(float x, float y, float w, float h) { /* screen coords */
    int x0 = (int)lroundf(G.offX + x * G.scale), x1 = (int)lroundf(G.offX + (x + w) * G.scale);
    int y0 = (int)lroundf(G.offY + y * G.scale), y1 = (int)lroundf(G.offY + (y + h) * G.scale);
    if (x1 <= x0 || y1 <= y0) return;
    SDL_Rect rc = { x0, y0, x1 - x0, y1 - y0 };
    SDL_RenderFillRect(G.ren, &rc);
}

/* Round a logical length to whole device pixels. */
static float snapPx(float v) { return roundf(v * G.scale) / G.scale; }

static void fillW(float x, float y, float w, float h) { /* world coords */
    fillS(x - G.camX + shakeX, y + shakeY, w, h);
}

static const char *glyph(char c) {
    switch (c) {
    case '0': return "111101101101111"; case '1': return "010110010010111";
    case '2': return "111001111100111"; case '3': return "111001111001111";
    case '4': return "101101111001001"; case '5': return "111100111001111";
    case '6': return "111100111101111"; case '7': return "111001001010010";
    case '8': return "111101111101111"; case '9': return "111101111001111";
    case 'A': return "010101111101101"; case 'B': return "110101110101110";
    case 'C': return "011100100100011"; case 'D': return "110101101101110";
    case 'E': return "111100110100111"; case 'F': return "111100110100100";
    case 'G': return "011100101101011"; case 'H': return "101101111101101";
    case 'I': return "111010010010111"; case 'J': return "001001001101010";
    case 'K': return "101101110101101"; case 'L': return "100100100100111";
    case 'M': return "101111111101101"; case 'N': return "110101101101101";
    case 'O': return "010101101101010"; case 'P': return "110101110100100";
    case 'Q': return "010101101110011"; case 'R': return "110101110101101";
    case 'S': return "011100010001110"; case 'T': return "111010010010010";
    case 'U': return "101101101101111"; case 'V': return "101101101101010";
    case 'W': return "101101111111101"; case 'X': return "101101010101101";
    case 'Y': return "101101010010010"; case 'Z': return "111001010100111";
    case ':': return "000010000010000"; case '-': return "000000111000000";
    case '!': return "010010010000010"; case '/': return "001001010100100";
    case '.': return "000000000000010"; case '(': return "010100100100010";
    case ')': return "010001001001010"; case '+': return "000010111010000";
    case ',': return "000000000010100"; case '\'': return "010010000000000";
    case '<': return "001010100010001"; case '>': return "100010001010100";
    case '%': return "101001010100101"; case '=': return "000111000111000";
    case '|': return "010010010010010";
    default: return NULL;
    }
}

static float textWidth(const char *s, float sc) { return (float)strlen(s) * 4 * sc - sc; }

static void textRaw(const char *s, float x, float y, float sc) {
    for (; *s; s++, x += 4 * sc) {
        const char *g = glyph((char)toupper((unsigned char)*s));
        if (!g) continue;
        for (int i = 0; i < 15; i++)
            if (g[i] == '1') fillS(x + (i % 3) * sc, y + (i / 3) * sc, sc, sc);
    }
}

static void text(const char *s, float x, float y, float sc, Uint8 r, Uint8 g, Uint8 b, bool center) {
    if (center) x -= textWidth(s, sc) / 2;
    color(20, 24, 40, 200);
    textRaw(s, x + sc * 0.6f, y + sc * 0.6f, sc);
    color(r, g, b, 255);
    textRaw(s, x, y, sc);
}

/* The player character, drawn in screen coords at scale `sc`. */
static void drawCharacter(float bx, float by, float sc, int facing, float squash, float step, float t) {
    float sq = squash * 0.25f;
    float w = PW * (1 + sq), h = PH * (1 - sq);
    float x0 = bx - (w - PW) / 2 * sc, y0 = by + (PH - h) * sc;
#define R(x, y, ww, hh) fillS(x0 + (x) * sc, y0 + (y) * sc, (ww) * sc, (hh) * sc)
    colorS(&PANTS[CFG.pants]); /* legs */
    R(3, h - 8 - fmaxf(0, step), 7, 8);
    R(w - 10, h - 8 - fmaxf(0, -step), 7, 8);
    colorS(&SHIRT[CFG.shirt]); /* body */
    R(0, 10, w, h - 17);
    color(255, 255, 255, 50);
    R(2, 11, w - 4, 3);
    colorS(&SKIN[CFG.skin]); /* head */
    R(2, 0, w - 4, 12);
    colorS(&HAIR[CFG.hair]);
    R(1, -2, w - 2, 4);
    R(facing > 0 ? 1 : w - 5, 0, 4, 5);
    float ex = facing > 0 ? w - 9 : 4;
    color(255, 255, 255, 255); R(ex, 3, 5, 5);
    color(20, 20, 40, 255); R(ex + (facing > 0 ? 2 : 0), 4, 3, 3);
    if (strcmp(SCARF[CFG.scarf].name, "NONE")) {
        colorS(&SCARF[CFG.scarf]);
        R(0, 10, w, 3);
        R(facing > 0 ? -6 : w, 11 + sinf(t * 14) * 1.5f, 6, 3);
    }
#undef R
}

/* Piecewise ridge height for background layers. */
static float ridge(float wx, float spacing, float lo, float hi, bool round, int seed) {
    float f = wx / spacing;
    int i = (int)floorf(f);
    float t = f - (float)i;
    if (round) t = (1 - cosf(t * 3.14159f)) / 2;
    float a = lo + hashf(i + seed) * (hi - lo);
    float b = lo + hashf(i + 1 + seed) * (hi - lo);
    return a + (b - a) * t;
}

static void drawBackground(void) {
    for (int i = 0; i < 27; i++) { /* sky gradient */
        float t = i / 26.0f;
        color((Uint8)(80 + 110 * t), (Uint8)(150 + 75 * t), (Uint8)(225 + 25 * t), 255);
        fillS(0, i * 20.0f, VW, 21);
    }
    color(255, 240, 190, 255); fillS(760, 60, 60, 60); /* sun */
    color(255, 250, 220, 255); fillS(770, 70, 40, 40);

    color(250, 252, 255, 255); /* clouds */
    float cx0 = snapPx(G.camX * 0.1f);
    for (int i = (int)floorf(cx0 / 280) - 1; i < (int)floorf((cx0 + VW) / 280) + 2; i++) {
        float x = i * 280 - cx0 + hashf(i * 7) * 120;
        float y = 40 + hashf(i * 13) * 120;
        float w = 70 + hashf(i * 3) * 60;
        fillS(x, y + 10, w, 18);
        fillS(x + 12, y, w * 0.55f, 14);
        fillS(x + w * 0.45f, y + 4, w * 0.35f, 10);
    }

    /* Columns sit at fixed spots in each layer and slide with it, so the
     * outline keeps its shape while scrolling. */
    float far = snapPx(G.camX * 0.2f), near = snapPx(G.camX * 0.45f);
    for (int k = (int)floorf(far / 3); k * 3 - far < VW; k++) {
        float h = ridge(k * 3.0f, 140, 140, 270, false, 11);
        color(130, 155, 205, 255); /* far mountains */
        fillS(k * 3 - far, VH - h, 3, h);
        if (h > 235) {
            color(235, 240, 250, 255); /* snow caps */
            fillS(k * 3 - far, VH - h, 3, (h - 235) * 0.8f);
        }
    }
    color(95, 170, 120, 255); /* near hills */
    for (int k = (int)floorf(near / 3); k * 3 - near < VW; k++) {
        float h = ridge(k * 3.0f, 220, 70, 160, true, 97);
        fillS(k * 3 - near, VH - h, 3, h);
    }
}

/* Two eyes looking where the enemy walks. */
static void eyes(float x, float y, float gap, float dir) {
    float d = dir > 0 ? 3 : -1;
    color(255, 255, 255, 255);
    fillW(x + d, y, 6, 7); fillW(x + gap + d, y, 6, 7);
    color(30, 20, 40, 255);
    fillW(x + 2 + d, y + 2, 3, 4); fillW(x + gap + 2 + d, y + 2, 3, 4);
}

static void drawEnemy(const Enemy *e) {
    float w, h, x = e->x, y = e->y;
    enemySize(e->kind, &w, &h);
    float wob = sinf(G.time * 12 + e->x) * 2;
    switch (e->kind) {
    case EN_WALKER: /* purple blob */
        color(110, 45, 130, 255); fillW(x, y + 4, w, h - 4);
        color(160, 80, 190, 255); fillW(x + 2, y + 2, w - 4, h - 8);
        color(90, 35, 105, 255);
        fillW(x + 3, y + h - 3 + wob * 0.5f, 7, 4);
        fillW(x + w - 10, y + h - 3 - wob * 0.5f, 7, 4);
        eyes(x + 6, y + 6, 9, e->vx);
        break;
    case EN_HOPPER: { /* green frog, legs stretch in the air */
        bool air = e->y < e->baseY - 1;
        color(40, 120, 50, 255);
        if (air) { fillW(x + 1, y + h - 2, 5, 8); fillW(x + w - 6, y + h - 2, 5, 8); }
        else { fillW(x - 3, y + h - 5, 9, 5); fillW(x + w - 6, y + h - 5, 9, 5); }
        color(70, 180, 80, 255); fillW(x, y + 5, w, h - 7);
        color(120, 220, 110, 255); fillW(x + 4, y + 11, w - 8, h - 15);
        color(70, 180, 80, 255); fillW(x + 2, y, 8, 8); fillW(x + w - 10, y, 8, 8);
        eyes(x + 3, y + 1, 11, e->vx);
        break;
    }
    case EN_FLYER: { /* bee with flapping wings */
        bool up = fmodf(G.time * 14 + e->x, 2) < 1;
        color(230, 240, 255, 200);
        fillW(x + 6, y + (up ? -8 : -2), 8, 7); fillW(x + 14, y + (up ? -8 : -2), 8, 7);
        color(250, 200, 40, 255); fillW(x, y + 3, w, h - 4);
        color(40, 30, 30, 255);
        fillW(x + 7, y + 3, 4, h - 4); fillW(x + 15, y + 3, 4, h - 4);
        fillW(e->vx > 0 ? x - 4 : x + w, y + h / 2, 4, 3); /* stinger */
        eyes(e->vx > 0 ? x + 16 : x + 1, y + 5, 0, e->vx);
        break;
    }
    case EN_SPIKY: /* red hedgehog: don't stomp */
        color(240, 240, 240, 255);
        for (int k = 0; k < 5; k++) {
            float sx = x + 2 + k * 5.5f;
            fillW(sx + 1, y - 6, 2, 4); fillW(sx, y - 2, 4, 4);
        }
        color(170, 35, 40, 255); fillW(x, y + 2, w, h - 2);
        color(220, 70, 60, 255); fillW(x + 2, y + 4, w - 4, h - 10);
        color(120, 25, 30, 255);
        fillW(x + 3, y + h - 3 + wob * 0.5f, 7, 4); fillW(x + w - 10, y + h - 3 - wob * 0.5f, 7, 4);
        eyes(x + 7, y + 7, 9, e->vx);
        break;
    case EN_ARMORED: /* beetle, steel helmet takes the first stomp */
        color(90, 60, 30, 255); fillW(x, y + 8, w, h - 8);
        color(150, 100, 55, 255); fillW(x + 2, y + 10, w - 4, h - 14);
        color(60, 40, 20, 255);
        fillW(x + 3, y + h - 3 + wob * 0.5f, 7, 4); fillW(x + w - 10, y + h - 3 - wob * 0.5f, 7, 4);
        if (e->hp > 1) {
            color(120, 130, 150, 255); fillW(x - 2, y, w + 4, 10);
            color(190, 200, 215, 255); fillW(x + 2, y + 1, w - 4, 3);
            eyes(x + 8, y + 11, 9, e->vx);
        } else { /* angry: red eyes, steam */
            eyes(x + 8, y + 11, 9, e->vx);
            color(230, 40, 40, 255); fillW(x + 8, y + 9, 15, 2);
            color(255, 255, 255, 120);
            fillW(x + 6, y - 6 - fmodf(G.time * 20, 6), 3, 3); fillW(x + w - 9, y - 4 - fmodf(G.time * 24, 6), 3, 3);
        }
        break;
    default: break;
    }
}

static void drawWorld(void) {
    float left = G.camX - 50, right = G.camX + VW + 50;

    for (int i = 0; i < G.nSolids; i++) {
        Solid *s = &G.solids[i];
        if (s->x + s->w < left || s->x > right) continue;
        if (s->ground) {
            color(140, 95, 60, 255); fillW(s->x, s->y, s->w, s->h);
            color(115, 75, 45, 255);
            int k0 = (int)floorf(s->x / 22);
            for (int k = k0; k * 22 < s->x + s->w - 6; k++) {
                if (k * 22.0f < s->x) continue;
                float hy = hashf(k * 31);
                if (hy < 0.55f) fillW(k * 22.0f + 4, s->y + 22 + hy * 90, 6, 4);
            }
            color(80, 185, 75, 255); fillW(s->x, s->y, s->w, 12);
            color(120, 215, 95, 255); fillW(s->x, s->y, s->w, 4);
            color(60, 145, 60, 255); fillW(s->x, s->y + 12, s->w, 3);
        } else {
            color(170, 120, 70, 255); fillW(s->x, s->y, s->w, s->h);
            color(205, 155, 95, 255); fillW(s->x, s->y, s->w, 4);
            color(120, 80, 45, 255); fillW(s->x, s->y + s->h - 3, s->w, 3);
            for (float bx = s->x + 18; bx < s->x + s->w - 6; bx += 24) fillW(bx, s->y + 4, 2, s->h - 7);
        }
    }

    for (int i = 0; i < G.nCoins; i++) {
        Coin *c = &G.coinArr[i];
        if (c->x < left || c->x > right) continue;
        float w = 2 + 12 * fabsf(cosf(G.time * 4 + c->x * 0.02f));
        float bob = sinf(G.time * 3 + c->x * 0.05f) * 2;
        color(200, 150, 30, 255); fillW(c->x - w / 2, c->y - 7 + bob, w, 14);
        color(255, 215, 60, 255); fillW(c->x - w / 2 + 1, c->y - 6 + bob, fmaxf(1, w - 2), 12);
        color(255, 250, 200, 255); fillW(c->x - w / 4, c->y - 4 + bob, fmaxf(1, w / 5), 5);
    }

    for (int i = 0; i < G.nEnemies; i++) {
        Enemy *e = &G.enemies[i];
        if (e->x < left || e->x > right) continue;
        drawEnemy(e);
    }

    bool blink = G.invuln > 0 && fmodf(G.time, 0.16f) < 0.08f;
    if (G.st != ST_OVER && !blink) {
        float step = G.onGround && fabsf(G.vx) > 20 ? sinf(G.runAnim) * 4 : 0;
        drawCharacter(G.px - G.camX + shakeX, G.py + shakeY, 1, G.facing, G.squash, step, G.time);
    }

    for (int i = 0; i < G.nParts; i++) {
        Part *p = &G.parts[i];
        color(p->r, p->g, p->b, (Uint8)(255 * clampf(p->life / p->max, 0, 1)));
        fillW(p->x, p->y, p->size, p->size);
    }
}

static void drawHUD(void) {
    char buf[96];
    float oy = G.dev ? 14 : 0; /* make room for the dev line */
    snprintf(buf, sizeof buf, "SCORE %d", G.score);
    text(buf, 20, 16 + oy, 4, 255, 255, 255, false);
    snprintf(buf, sizeof buf, "COINS %d", G.coins);
    text(buf, 20, 44 + oy, 3, 255, 225, 90, false);
    if (G.best > 0) {
        snprintf(buf, sizeof buf, "BEST %d", G.best);
        text(buf, VW - 20 - textWidth(buf, 3), 16 + oy, 3, 230, 240, 255, false);
    }
    text(DIFFS[G.runDiff].name, VW - 20 - textWidth(DIFFS[G.runDiff].name, 2), 70 + oy, 2, 200, 210, 235, false);
    for (int i = 0; i < G.maxLives; i++) {
        float x = VW - 20 - (G.maxLives - i) * 26.0f, y = 42 + oy;
        color(20, 24, 40, 160); fillS(x + 2, y + 2, 20, 18);
        if (i < G.lives) color(235, 60, 70, 255); else color(90, 90, 110, 255);
        fillS(x, y + 4, 20, 8); fillS(x + 2, y, 6, 6); fillS(x + 12, y, 6, 6);
        fillS(x + 3, y + 12, 14, 3); fillS(x + 7, y + 15, 6, 3);
    }
    if (G.pad) {
        snprintf(buf, sizeof buf, "PAD: %s", SDL_GameControllerName(G.pad));
        text(buf, 20, VH - 26, 2, 200, 255, 200, false);
    } else {
        text("NO CONTROLLER - USING KEYBOARD", 20, VH - 26, 2, 255, 220, 200, false);
    }
}

static void overlay(Uint8 a) {
    color(10, 14, 30, a);
    fillS(0, 0, VW, VH);
}

static void drawMenu(const char *const *items, int n, int sel, float y, float sc) {
    for (int i = 0; i < n; i++) {
        float yy = y + i * (sc * 5 + 14);
        if (i == sel) {
            char buf[64];
            snprintf(buf, sizeof buf, "> %s <", items[i]);
            text(buf, VW / 2, yy, sc, 255, 220, 80, true);
        } else {
            text(items[i], VW / 2, yy, sc, 220, 225, 245, true);
        }
    }
}

static void drawHint(const char *s) { text(s, VW / 2, VH - 30, 2, 170, 180, 210, true); }

#define TITLE_ITEMS 4 /* START, DIFFICULTY, SETTINGS, QUIT */
static const char *const PAUSE_MENU[] = { "RESUME", "RESTART", "SETTINGS", "MAIN MENU", "QUIT GAME" };

enum { SET_HAIR, SET_SKIN, SET_SHIRT, SET_PANTS, SET_SCARF, SET_SFX, SET_MUSIC, SET_VOLUME,
       SET_RUMBLE, SET_FPS, SET_BACK, SET_COUNT };
static const char *const SETTING_NAMES[SET_COUNT] = {
    "HAIR", "SKIN", "SHIRT", "PANTS", "SCARF", "SOUND EFFECTS", "MUSIC", "VOLUME", "RUMBLE", "FPS LIMIT", "BACK",
};

static const Swatch *settingSwatch(int i) {
    switch (i) {
    case SET_HAIR: return &HAIR[CFG.hair];
    case SET_SKIN: return &SKIN[CFG.skin];
    case SET_SHIRT: return &SHIRT[CFG.shirt];
    case SET_PANTS: return &PANTS[CFG.pants];
    case SET_SCARF: return &SCARF[CFG.scarf];
    default: return NULL;
    }
}

static void drawSettings(void) {
    overlay(190);
    text("SETTINGS", VW / 2, 28, 7, 255, 220, 80, true);

    float y0 = 88, dy = 33;
    for (int i = 0; i < SET_COUNT; i++) {
        float y = y0 + i * dy + (i == SET_BACK ? 10 : 0);
        bool sel = i == G.settingsSel;
        Uint8 r = sel ? 255 : 220, g = sel ? 220 : 225, b = sel ? 80 : 245;
        if (sel) { color(255, 255, 255, 30); fillS(70, y - 6, 560, 30); }
        if (i == SET_BACK) { text(sel ? "> BACK <" : "BACK", 350, y, 3, r, g, b, true); continue; }
        text(SETTING_NAMES[i], 90, y, 3, r, g, b, false);
        char val[32];
        const Swatch *sw = settingSwatch(i);
        if (sw) snprintf(val, sizeof val, "%s", sw->name);
        else if (i == SET_SFX) snprintf(val, sizeof val, "%s", CFG.sfx ? "ON" : "OFF");
        else if (i == SET_MUSIC) snprintf(val, sizeof val, "%s", CFG.music ? "ON" : "OFF");
        else if (i == SET_RUMBLE) snprintf(val, sizeof val, "%s", CFG.rumble ? "ON" : "OFF");
        else if (i == SET_FPS) snprintf(val, sizeof val, "%d", FPS_OPTIONS[CFG.fps]);
        else snprintf(val, sizeof val, "%d%%", CFG.volume * 10);
        char shown[48];
        snprintf(shown, sizeof shown, sel ? "< %s >" : "%s", val);
        text(shown, 470, y, 3, r, g, b, true);
        if (sw && strcmp(sw->name, "NONE")) {
            color(20, 24, 40, 255); fillS(588, y - 2, 22, 19);
            colorS(sw); fillS(590, y, 18, 15);
        }
        if (i == SET_FPS) {
            SDL_DisplayMode m;
            int hz = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(G.win), &m) == 0 ? m.refresh_rate : 0;
            if (hz > 0 && FPS_OPTIONS[CFG.fps] > hz) {
                char note[48];
                snprintf(note, sizeof note, "THIS SCREEN SHOWS MAX %d", hz);
                text(note, 470, y + 19, 1, 255, 170, 90, true);
            }
        }
        if (i == SET_VOLUME) {
            for (int k = 0; k < 10; k++) {
                if (k < CFG.volume) color(120, 220, 120, 255); else color(70, 75, 95, 255);
                fillS(540 + k * 9, y + 18, 7, 4);
            }
        }
    }

    /* Live character preview */
    color(255, 255, 255, 25); fillS(680, 110, 220, 260);
    color(80, 185, 75, 255); fillS(700, 330, 180, 12);
    color(140, 95, 60, 255); fillS(700, 342, 180, 18);
    int facing = fmodf(G.time, 4.0f) < 2.0f ? 1 : -1;
    float bob = fabsf(sinf(G.time * 3)) * 14;
    drawCharacter(790 - PW * 2.5f, 330 - PH * 5 - bob, 5, facing, 0, sinf(G.time * 8) * 3, G.time);
    text("PREVIEW", 790, 380, 2, 200, 205, 230, true);

    drawHint("UP/DOWN: CHOOSE    LEFT/RIGHT: CHANGE    CIRCLE / ESC: BACK");
}

/* ------------------------------------------------------------ dev mode */

static double cpuSeconds(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static float peakMemMB(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (float)ru.ru_maxrss / (1024 * 1024); /* bytes on macOS */
#else
    return (float)ru.ru_maxrss / 1024;          /* kilobytes on Linux/BSD */
#endif
}

/* One-line performance overlay along the top-left edge. */
static void drawDev(void) {
    char pad[40] = "PAD NONE";
    if (G.pad) {
        SDL_JoystickPowerLevel pl = SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(G.pad));
        snprintf(pad, sizeof pad, "PAD %s", pl == SDL_JOYSTICK_POWER_WIRED ? "WIRED"
                 : pl == SDL_JOYSTICK_POWER_FULL ? "BAT FULL" : pl == SDL_JOYSTICK_POWER_MEDIUM ? "BAT MED"
                 : pl == SDL_JOYSTICK_POWER_LOW ? "BAT LOW" : "OK");
    }
    char line[200];
    snprintf(line, sizeof line,
             "FPS %.0f | FRAME %.1fMS (MAX %.1f) | CPU %.0f%% | UPD %.2fMS | DRAW %.2fMS | MEM %.0fMB | PHYS %d/F | OBJ %d | %s",
             G.fps, G.frameMs, G.frameMsMax, G.cpuPct, G.updMs, G.drawMs, G.memMB, G.stepsThisFrame,
             G.nSolids + G.nCoins + G.nEnemies + G.nParts, pad);
    color(0, 0, 0, 170);
    fillS(0, 0, textWidth(line, 2) + 12, 16);
    Uint8 r = G.fps >= 55 ? 120 : G.fps >= 30 ? 255 : 255, g = G.fps >= 55 ? 255 : G.fps >= 30 ? 220 : 90;
    color(r, g, 120, 255);
    textRaw(line, 6, 3, 2);
}

static void render(void) {
    shakeX = shakeY = 0;
    if (G.shake > 0) {
        shakeX = snapPx((frand() - 0.5f) * 14 * G.shake / 0.3f);
        shakeY = snapPx((frand() - 0.5f) * 14 * G.shake / 0.3f);
    }
    drawBackground();
    drawWorld();

    switch (G.st) {
    case ST_TITLE:
        overlay(140);
        text("DS3 JUMPER", VW / 2, 80, 12, 255, 220, 80, true);
        text("A TINY PLATFORMER FOR THE PS3 CONTROLLER", VW / 2, 160, 3, 255, 255, 255, true);
        {
            char diff[40];
            snprintf(diff, sizeof diff, "DIFFICULTY: %s", DIFFS[CFG.difficulty].name);
            const char *items[TITLE_ITEMS] = { "START", diff, "SETTINGS", "QUIT" };
            drawMenu(items, TITLE_ITEMS, G.menuSel, 215, 5);
            if (G.menuSel == 1) {
                static const char *const about[DIFF_COUNT] = {
                    "5 LIVES - FEWER, SLOWER ENEMIES",
                    "3 LIVES - THE NORMAL GAME",
                    "2 LIVES - FAST ENEMIES, EVERY TYPE EARLY",
                };
                char line[96];
                snprintf(line, sizeof line, "%s   (LEFT/RIGHT TO CHANGE)", about[CFG.difficulty]);
                text(line, VW / 2, 372, 2, 200, 210, 235, true);
            }
        }
        if (S.games > 0) {
            char buf[160];
            snprintf(buf, sizeof buf, "BEST %s %d   LAST %d   GAMES %d   COINS %d   STOMPS %d",
                     DIFFS[CFG.difficulty].name, S.bestD[CFG.difficulty], S.last, S.games,
                     S.coinsTotal, S.stompsTotal);
            text(buf, VW / 2, 400, 2, 255, 225, 120, true);
        }
        text("STICK/D-PAD MOVE   CROSS JUMP   SQUARE/R2 RUN   START PAUSE   TRIANGLE FULLSCREEN",
             VW / 2, 440, 2, 230, 235, 255, true);
        drawHint("UP/DOWN: CHOOSE    CROSS / ENTER: OK");
        break;
    case ST_PLAY:
        drawHUD();
        break;
    case ST_PAUSE:
        drawHUD();
        overlay(150);
        text("PAUSED", VW / 2, 90, 10, 255, 255, 255, true);
        drawMenu(PAUSE_MENU, LEN(PAUSE_MENU), G.menuSel, 200, 4);
        drawHint("UP/DOWN: CHOOSE    CROSS: OK    START / CIRCLE: RESUME");
        break;
    case ST_OVER:
        drawHUD();
        overlay(160);
        text("GAME OVER", VW / 2, 120, 10, 255, 110, 100, true);
        {
            char buf[64];
            snprintf(buf, sizeof buf, "SCORE %d   BEST %d (%s)", G.score, G.best, DIFFS[G.runDiff].name);
            text(buf, VW / 2, 220, 4, 255, 255, 255, true);
        }
        if (G.newBest && fmodf(G.time, 0.5f) < 0.35f)
            text("NEW BEST!", VW / 2, 265, 5, 255, 220, 80, true);
        if (fmodf(G.time, 1.0f) < 0.65f)
            text("PRESS CROSS TO PLAY AGAIN", VW / 2, 320, 3, 255, 255, 255, true);
        drawHint("CIRCLE / ESC: MAIN MENU");
        break;
    case ST_SETTINGS:
        drawSettings();
        break;
    }
    if (G.dev) drawDev();
}

/* ------------------------------------------------------------ menus */

static void openSettings(State from) {
    G.settingsReturn = from;
    G.settingsSel = 0;
    G.st = ST_SETTINGS;
}

static int cycle(int v, int n, int d) { return ((v + d) % n + n) % n; }

static void updateSettings(const Input *in) {
    if (in->upP) { G.settingsSel = cycle(G.settingsSel, SET_COUNT, -1); audio_play(SFX_MENU_MOVE); }
    if (in->downP) { G.settingsSel = cycle(G.settingsSel, SET_COUNT, 1); audio_play(SFX_MENU_MOVE); }
    int d = in->leftP ? -1 : in->rightP ? 1 : 0;
    if (in->confirmP && G.settingsSel != SET_BACK) d = 1;
    if (d) {
        switch (G.settingsSel) {
        case SET_HAIR: CFG.hair = cycle(CFG.hair, LEN(HAIR), d); break;
        case SET_SKIN: CFG.skin = cycle(CFG.skin, LEN(SKIN), d); break;
        case SET_SHIRT: CFG.shirt = cycle(CFG.shirt, LEN(SHIRT), d); break;
        case SET_PANTS: CFG.pants = cycle(CFG.pants, LEN(PANTS), d); break;
        case SET_SCARF: CFG.scarf = cycle(CFG.scarf, LEN(SCARF), d); break;
        case SET_SFX: CFG.sfx = !CFG.sfx; break;
        case SET_MUSIC: CFG.music = !CFG.music; break;
        case SET_VOLUME: CFG.volume = (int)clampf((float)(CFG.volume + d), 0, 10); break;
        case SET_RUMBLE:
            CFG.rumble = !CFG.rumble;
            rumble(0x6000, 0x6000, 200); /* feel it when turned on */
            break;
        case SET_FPS: CFG.fps = cycle(CFG.fps, LEN(FPS_OPTIONS), d); break;
        default: break;
        }
        if (G.settingsSel != SET_BACK) {
            applyAudioConfig();
            audio_play(SFX_TOGGLE);
            writeSettings();
        }
    }
    if (in->backP || in->startP || (in->confirmP && G.settingsSel == SET_BACK)) {
        writeSettings();
        audio_play(SFX_MENU_BACK);
        G.st = G.settingsReturn;
    }
}

/* ----------------------------------------------------------------- main */

/* Draw the frame `alpha` (0..1) of the way between the last two physics
 * steps, with the camera and moving things snapped to whole device pixels.
 * Physics runs at 120 steps/s; this keeps motion smooth at any frame rate. */
static void renderFrame(float alpha) {
    int ow = VW, oh = VH;
    SDL_GetRendererOutputSize(G.ren, &ow, &oh);
    G.scale = fminf(ow / (float)VW, oh / (float)VH);
    G.offX = roundf((ow - VW * G.scale) / 2);
    G.offY = roundf((oh - VH * G.scale) / 2);
    SDL_Rect clip = { (int)G.offX, (int)G.offY, (int)lroundf(VW * G.scale), (int)lroundf(VH * G.scale) };
    SDL_RenderSetClipRect(G.ren, &clip);

    static float ex[MAX_ENEMIES], ey[MAX_ENEMIES];
    float cam = G.camX, px = G.px, py = G.py;
    #define LERP(a, b) ((a) + ((b) - (a)) * alpha)
    G.camX = snapPx(LERP(G.prevCamX, cam));
    G.px = G.camX + snapPx(LERP(G.prevPx, px) - G.camX);
    G.py = snapPx(LERP(G.prevPy, py));
    for (int i = 0; i < G.nEnemies; i++) {
        Enemy *e = &G.enemies[i];
        ex[i] = e->x; ey[i] = e->y;
        e->x = G.camX + snapPx(LERP(e->prevX, e->x) - G.camX);
        e->y = snapPx(LERP(e->prevY, e->y));
    }
    #undef LERP
    render();
    G.camX = cam; G.px = px; G.py = py;
    for (int i = 0; i < G.nEnemies; i++) { G.enemies[i].x = ex[i]; G.enemies[i].y = ey[i]; }
}

static int displayHz(void) {
    SDL_DisplayMode m;
    if (!G.win || SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(G.win), &m) != 0) return 0;
    return m.refresh_rate;
}

/* Wait until the next frame is due for the chosen FPS limit. Sleeps most of
 * the wait and spins the last ~1.5 ms, since SDL_Delay is only ms-accurate. */
static void limitFrame(Uint64 *next, int hz) {
    const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64 period = freq / (Uint64)FPS_OPTIONS[CFG.fps];
    *next += period;
    /* vsync is on, so the next present waits for the screen anyway: wake half a
     * refresh early and let it line the frame up with the screen. */
    Uint64 until = *next - (hz > 0 ? freq / (Uint64)hz / 2 : 0);
    Uint64 now = SDL_GetPerformanceCounter();
    if (now >= until) {                    /* running late: don't try to catch up */
        if (now > *next + period) *next = now;
        return;
    }
    Uint64 spin = freq * 3 / 2000;
    if (until - now > spin) SDL_Delay((Uint32)((until - now - spin) * 1000 / freq));
    while (SDL_GetPerformanceCounter() < until) {}
}

static void setFullscreen(bool on) {
    G.fullscreen = on;
    SDL_SetWindowFullscreen(G.win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
}

/* Headless: simulate a few seconds of scripted play and save a BMP. */
static void renderFrame(float alpha);

static int screenshot(const char *path, State view) {
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, VW, VH, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) { fprintf(stderr, "surface: %s\n", SDL_GetError()); return 1; }
    G.ren = SDL_CreateSoftwareRenderer(surf);
    if (!G.ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);
    newGame();
    G.rng = 12345;
    Input in = { 0 };
    for (int f = 0; f < 120 * 4; f++) {
        in.move = 1; in.runHeld = true;
        in.jumpHeld = (f % 70) < 30;
        if (f % 70 == 0) G.jumpBuf = 0.12f;
        if (G.st == ST_PLAY) update(STEP, &in);
        updateParticles(STEP);
        G.time += STEP;
    }
    endRun();
    if (view == ST_SETTINGS) openSettings(ST_TITLE);
    else if (view != ST_PLAY) G.st = view;
    renderFrame(1);
    SDL_RenderPresent(G.ren);
    int rc = SDL_SaveBMP(surf, path);
    printf(rc == 0 ? "saved %s (score %d)\n" : "failed to save %s\n", path, G.score);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    bool windowed = false, stats = false;
    const char *shot = NULL;
    State shotView = ST_PLAY;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windowed") || !strcmp(argv[i], "-w")) windowed = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) {
            const char *v = argv[++i];
            shotView = !strcmp(v, "title") ? ST_TITLE : !strcmp(v, "pause") ? ST_PAUSE
                     : !strcmp(v, "settings") ? ST_SETTINGS : !strcmp(v, "over") ? ST_OVER : ST_PLAY;
        } else if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--dev")) G.dev = true;
        else {
            printf("usage: %s [--windowed] [--dev] [--stats] [--screenshot out.bmp [--screen title|play|pause|settings|over]]\n",
                   argv[0]);
            return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 2;
        }
    }

    initPaths();
    loadSave();
    loadSettings();
    G.best = S.best;

    if (stats) {
        printf("save file:     %s\nsettings file: %s\n\n", savePath[0] ? savePath : "(unavailable)",
               settingsPath[0] ? settingsPath : "(unavailable)");
        FILE *f = savePath[0] ? fopen(savePath, "r") : NULL;
        if (!f) { printf("no saved games yet - play one first!\n"); return 0; }
        char line[256];
        while (fgets(line, sizeof line, f)) fputs(line, stdout);
        fclose(f);
        return 0;
    }

    if (shot) {
        /* Don't touch the real save unless a test file is given explicitly. */
        G.noSave = !getenv("DS3_JUMPER_SAVE");
        if (SDL_Init(0) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        int rc = screenshot(shot, shotView);
        SDL_Quit();
        return rc;
    }

#ifdef __APPLE__
    /* macOS has no DS3 driver; SDL's HIDAPI driver activates it over USB. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!audio_init()) fprintf(stderr, "no audio device: %s (continuing without sound)\n", SDL_GetError());
    applyAudioConfig();

    G.win = SDL_CreateWindow("DS3 Jumper", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             VW, VH, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!G.win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    /* vsync stops tearing; lower FPS limits are paced by limitFrame on top */
    G.ren = SDL_CreateRenderer(G.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!G.ren) G.ren = SDL_CreateRenderer(G.win, -1, 0);
    if (!G.ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);
    if (!windowed) setFullscreen(true);

    for (int i = 0; i < SDL_NumJoysticks(); i++) openPad(i);

    if (S.games > 0)
        printf("Best scores: EASY %d   MEDIUM %d   HARD %d  (%d games played)\n",
               S.bestD[DIFF_EASY], S.bestD[DIFF_MEDIUM], S.bestD[DIFF_HARD], S.games);
    else
        printf("No best score yet - go set one!\n");
    fflush(stdout);

    toTitle();

    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 nextFrame = last;
    float acc = 0;
    Input in;

    while (!G.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: G.quit = true; break;
            case SDL_CONTROLLERDEVICEADDED: openPad(e.cdevice.which); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (G.pad && e.cdevice.which ==
                                 SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(G.pad))) {
                    SDL_GameControllerClose(G.pad);
                    G.pad = NULL;
                    for (int i = 0; i < SDL_NumJoysticks(); i++) openPad(i);
                    if (G.st == ST_PLAY) { G.st = ST_PAUSE; G.menuSel = 0; }
                }
                break;
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)SDL_GetPerformanceFrequency();
        last = now;

        /* FPS / frame-time stats for dev mode, measured on the real frame time */
        G.fpsFrames++;
        G.fpsTimer += dt;
        G.frameMsMaxAcc = fmaxf(G.frameMsMaxAcc, dt * 1000);
        if (G.fpsTimer >= 0.5f) {
            double cpu = cpuSeconds();
            G.fps = G.fpsFrames / G.fpsTimer;
            G.frameMs = G.fpsTimer * 1000 / G.fpsFrames;
            G.frameMsMax = G.frameMsMaxAcc;
            G.updMs = G.updAcc / G.fpsFrames;
            G.drawMs = G.drawAcc / G.fpsFrames;
            G.cpuPct = (float)((cpu - G.lastCpuSec) / G.fpsTimer * 100);
            G.lastCpuSec = cpu;
            G.memMB = peakMemMB();
            G.fpsFrames = 0; G.fpsTimer = 0; G.frameMsMaxAcc = 0; G.updAcc = 0; G.drawAcc = 0;
        }
        if (dt > 0.05f) dt = 0.05f;
        /* Snap a frame time that is within 1.5 ms of the expected one to exactly
         * that, so every 60 FPS frame runs exactly 2 physics steps (not 1-3-2...). */
        int hz = displayHz();
        int target = FPS_OPTIONS[CFG.fps] < hz || hz <= 0 ? FPS_OPTIONS[CFG.fps] : hz;
        if (fabsf(dt - 1.0f / target) < 0.0015f) dt = 1.0f / target;

        readInput(&in, dt);
        if (in.triP) setFullscreen(!G.fullscreen);
        if (in.selectP) { G.dev = !G.dev; audio_play(SFX_TOGGLE); }
        G.stepsThisFrame = 0;

        switch (G.st) {
        case ST_TITLE:
            if (in.upP) { G.menuSel = cycle(G.menuSel, TITLE_ITEMS, -1); audio_play(SFX_MENU_MOVE); }
            if (in.downP) { G.menuSel = cycle(G.menuSel, TITLE_ITEMS, 1); audio_play(SFX_MENU_MOVE); }
            if (G.menuSel == 1 && (in.leftP || in.rightP || in.confirmP)) {
                CFG.difficulty = cycle(CFG.difficulty, DIFF_COUNT, in.leftP ? -1 : 1);
                audio_play(SFX_TOGGLE);
                writeSettings();
            } else if (in.psP) G.quit = true;
            else if (in.confirmP || in.startP) {
                if (G.menuSel == 0) newGame();
                else if (G.menuSel == 2) { audio_play(SFX_MENU_OK); openSettings(ST_TITLE); }
                else G.quit = true;
            }
            break;
        case ST_PLAY:
            if (in.startP || in.psP) {
                G.st = ST_PAUSE; G.menuSel = 0;
                audio_play(SFX_MENU_OK);
                break;
            }
            if (in.jumpP) G.jumpBuf = 0.12f;
            acc += dt;
            Uint64 u0 = SDL_GetPerformanceCounter();
            while (acc >= STEP * 0.999f && G.st == ST_PLAY) {
                update(STEP, &in);
                acc -= STEP;
                G.stepsThisFrame++;
            }
            G.updAcc += (float)(SDL_GetPerformanceCounter() - u0) * 1000 / (float)SDL_GetPerformanceFrequency();
            break;
        case ST_PAUSE:
            acc = 0;
            if (in.upP) { G.menuSel = cycle(G.menuSel, LEN(PAUSE_MENU), -1); audio_play(SFX_MENU_MOVE); }
            if (in.downP) { G.menuSel = cycle(G.menuSel, LEN(PAUSE_MENU), 1); audio_play(SFX_MENU_MOVE); }
            if (in.confirmP) {
                switch (G.menuSel) {
                case 0: G.st = ST_PLAY; audio_play(SFX_MENU_BACK); break;
                case 1: newGame(); break;
                case 2: audio_play(SFX_MENU_OK); openSettings(ST_PAUSE); break;
                case 3: audio_play(SFX_MENU_BACK); toTitle(); break;
                default: G.quit = true; break;
                }
            } else if (in.startP || in.backP) {
                G.st = ST_PLAY;
                audio_play(SFX_MENU_BACK);
            } else if (in.psP) G.quit = true;
            break;
        case ST_OVER:
            if (in.backP) { audio_play(SFX_MENU_BACK); toTitle(); } /* before start: ESC is both */
            else if (in.confirmP || in.startP) newGame();
            else if (in.psP) G.quit = true;
            break;
        case ST_SETTINGS:
            updateSettings(&in);
            break;
        }

        if (G.st != ST_PAUSE && G.st != ST_SETTINGS) updateParticles(dt);
        G.time += dt;
        G.shake = fmaxf(0, G.shake - dt);

        Uint64 r0 = SDL_GetPerformanceCounter();
        color(0, 0, 0, 255);
        SDL_RenderClear(G.ren);
        renderFrame(G.st == ST_PLAY ? clampf(acc / STEP, 0, 1) : 1);
        G.drawAcc += (float)(SDL_GetPerformanceCounter() - r0) * 1000 / (float)SDL_GetPerformanceFrequency();
        SDL_RenderPresent(G.ren);
        if (FPS_OPTIONS[CFG.fps] < hz || hz <= 0) limitFrame(&nextFrame, hz);
        else nextFrame = SDL_GetPerformanceCounter();
    }

    endRun(); /* save a run that was in progress when quitting */
    writeSettings();
    if (G.playedThisSession) {
        printf("Last score: %d (%s)   Best on %s: %d%s\n", S.last, DIFFS[G.runDiff].name,
               DIFFS[G.runDiff].name, S.bestD[G.runDiff],
               G.sessionNewBest ? "  (NEW BEST this session!)" : "");
        printf("Stats: %s\n", savePath);
    }
    audio_quit();
    if (G.pad) SDL_GameControllerClose(G.pad);
    SDL_DestroyRenderer(G.ren);
    SDL_DestroyWindow(G.win);
    SDL_Quit();
    return 0;
}
