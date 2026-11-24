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
#define START_LIVES 3

#define MAX_SOLIDS 256
#define MAX_COINS 512
#define MAX_ENEMIES 64
#define MAX_PARTS 512

typedef struct { float x, y, w, h; bool ground; } Solid;
typedef struct { float x, y; } Coin;
typedef struct { float x, y, vx, minx, maxx; } Enemy;
typedef struct { float x, y, vx, vy, life, max, size; Uint8 r, g, b; } Part;

typedef enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER } State;

typedef struct {
    bool jump, run, start, select, tri, ps;
} Held;

typedef struct {
    float move;
    bool jumpHeld, runHeld;
    bool jumpP, startP, selectP, triP, psP;
} Input;

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_GameController *pad;
    bool fullscreen, quit;
    State st;

    float px, py, vx, vy;
    bool onGround;
    float coyote, jumpBuf, invuln, squash, runAnim;
    int facing;
    float safeX, safeY;

    int lives, coins, bonus, score, best, stomps;
    bool runActive, noSave;
    float runSeconds;
    float dist, camX, time, shake;

    Solid solids[MAX_SOLIDS]; int nSolids;
    Coin coinArr[MAX_COINS]; int nCoins;
    Enemy enemies[MAX_ENEMIES]; int nEnemies;
    Part parts[MAX_PARTS]; int nParts;

    float genX, genY;
    Uint32 rng;
    Held prev;
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
    if (G.pad) SDL_GameControllerRumble(G.pad, lo, hi, ms);
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

static void addEnemy(float minx, float maxx, float y) {
    if (G.nEnemies >= MAX_ENEMIES || maxx <= minx) return;
    float x = minx + frand() * (maxx - minx);
    G.enemies[G.nEnemies++] = (Enemy){ x, y, frand() < 0.5f ? -60.0f : 60.0f, minx, maxx };
}

/* Time in the air for a full jump that lands `rise` px higher (negative = lower). */
static float airTime(float rise) {
    float d = JUMP_V * JUMP_V - 2 * GRAV * rise;
    if (d < 0) return 0;
    return (JUMP_V + sqrtf(d)) / GRAV;
}

static void genChunk(void) {
    float diff = clampf(G.genX / 25000.0f, 0, 1);

    float dy = (frand() - 0.5f) * 2 * (40 + 40 * diff);
    float newY = clampf(G.genY + dy, 300, 470);
    float rise = G.genY - newY;
    if (rise > 80) { newY = G.genY - 80; rise = 80; }

    float gap = 40 + frand() * (60 + 120 * diff);
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

    if (w > 280 && frand() < 0.3f + 0.45f * diff)
        addEnemy(x + 20, x + w - 20 - EW, newY - EH);

    if (frand() < 0.45f) {
        float fw = 80 + frand() * 90;
        float fx = x + frand() * fmaxf(1, w - fw);
        float fy = newY - 105 - frand() * 25;
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
        if (G.enemies[i].maxx + EW > edge) G.enemies[j++] = G.enemies[i];
    G.nEnemies = j;
}

/* ------------------------------------------------------------ save file */

/* Plain "key=value" text file in the per-user data directory. */
static struct {
    int best, last, games, coinsTotal, bestCoins, stompsTotal;
    double distanceTotal, playSeconds;
} S;
static char savePath[1024];

static void initSavePath(void) {
    const char *env = getenv("DS3_JUMPER_SAVE");
    if (env && *env) { snprintf(savePath, sizeof savePath, "%s", env); return; }
    char *dir = SDL_GetPrefPath("danidomi", "ds3-jumper"); /* creates the folder */
    if (dir) {
        snprintf(savePath, sizeof savePath, "%ssave.txt", dir);
        SDL_free(dir);
    }
}

static void loadSave(void) {
    FILE *f = savePath[0] ? fopen(savePath, "r") : NULL;
    if (!f) return;
    char line[256], key[64];
    double v;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %lf", key, &v) != 2) continue;
        if (!strcmp(key, "best_score")) S.best = (int)v;
        else if (!strcmp(key, "last_score")) S.last = (int)v;
        else if (!strcmp(key, "games_played")) S.games = (int)v;
        else if (!strcmp(key, "coins_total")) S.coinsTotal = (int)v;
        else if (!strcmp(key, "best_coins")) S.bestCoins = (int)v;
        else if (!strcmp(key, "enemies_stomped")) S.stompsTotal = (int)v;
        else if (!strcmp(key, "distance_total")) S.distanceTotal = v;
        else if (!strcmp(key, "play_seconds")) S.playSeconds = v;
    }
    fclose(f);
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
    if (G.score > S.best) S.best = G.score;
    S.coinsTotal += G.coins;
    if (G.coins > S.bestCoins) S.bestCoins = G.coins;
    S.stompsTotal += G.stomps;
    S.distanceTotal += G.dist;
    S.playSeconds += G.runSeconds;
    G.best = S.best;
    writeSave();
}

/* --------------------------------------------------------------- game */

static void newGame(void) {
    endRun();
    G.nSolids = G.nCoins = G.nEnemies = G.nParts = 0;
    G.rng = (SDL_GetTicks() * 2654435761u) | 1u;
    addSolid(-300, 420, 1300, VH + 300 - 420, true);
    G.genX = 1000; G.genY = 420;
    G.px = 120; G.py = 420 - PH; G.vx = G.vy = 0;
    G.onGround = true; G.facing = 1;
    G.coyote = G.jumpBuf = G.invuln = G.squash = 0;
    G.safeX = G.px; G.safeY = G.py;
    G.camX = 0; G.dist = 0;
    G.lives = START_LIVES; G.coins = G.bonus = G.score = G.stomps = 0;
    G.shake = 0;
    G.runActive = true; G.runSeconds = 0;
    G.st = ST_PLAY;
    if (G.pad) SDL_GameControllerSetPlayerIndex(G.pad, 0);
}

static void gameOver(void) {
    G.st = ST_OVER;
    endRun();
}

static void hurt(void) {
    G.lives--;
    G.shake = 0.3f;
    rumble(0xFFFF, 0xFFFF, 350);
    spawn(G.px + PW / 2, G.py + PH / 2, 24, 230, 70, 70, 260, 120);
    if (G.lives <= 0) { gameOver(); return; }
    G.invuln = 1.5f;
}

static void fell(void) {
    hurt();
    if (G.st != ST_PLAY) return;
    G.px = G.safeX; G.py = G.safeY - 40;
    G.vx = G.vy = 0;
    if (G.camX > G.px - 120) G.camX = G.px - 120;
    for (int i = 0; i < G.nEnemies; i++)
        if (fabsf(G.enemies[i].x - G.px) < 180) G.enemies[i] = G.enemies[--G.nEnemies], i--;
}

static void update(float dt, const Input *in) {
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
        e->x += e->vx * dt;
        if (e->x < e->minx) { e->x = e->minx; e->vx = fabsf(e->vx); }
        if (e->x > e->maxx) { e->x = e->maxx; e->vx = -fabsf(e->vx); }
        if (!overlap(G.px, G.py, PW, PH, e->x, e->y, EW, EH)) continue;
        if (G.vy > 0 && G.py + PH - e->y < 16) {
            spawn(e->x + EW / 2, e->y + EH / 2, 18, 160, 80, 190, 220, 80);
            G.enemies[i] = G.enemies[--G.nEnemies]; i--;
            G.vy = in->jumpHeld ? -620 : -420;
            G.bonus += 50;
            G.stomps++;
            rumble(0x6000, 0, 90);
        } else if (G.invuln <= 0) {
            G.vy = -380;
            G.vx = (G.px < e->x) ? -250 : 250;
            hurt();
            if (G.st != ST_PLAY) return;
        }
    }

    /* Coins */
    for (int i = 0; i < G.nCoins; i++) {
        Coin *c = &G.coinArr[i];
        if (!overlap(G.px, G.py, PW, PH, c->x - 7, c->y - 7, 14, 14)) continue;
        spawn(c->x, c->y, 8, 255, 220, 70, 120, 60);
        G.coinArr[i] = G.coinArr[--G.nCoins]; i--;
        G.coins++;
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

static void readInput(Input *in) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    Held h = { 0 };
    float mv = 0;

    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) mv -= 1;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) mv += 1;
    h.jump = k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W] || k[SDL_SCANCODE_Z];
    h.run = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_X];
    h.start = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_P];
    h.select = k[SDL_SCANCODE_R];
    h.tri = k[SDL_SCANCODE_F] || k[SDL_SCANCODE_F11];
    h.ps = k[SDL_SCANCODE_Q];

    if (G.pad) {
        float ax = axis(SDL_CONTROLLER_AXIS_LEFTX);
        if (fabsf(ax) > 0.2f) mv += (ax - copysignf(0.2f, ax)) / 0.8f;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mv -= 1;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mv += 1;
        h.jump |= btn(SDL_CONTROLLER_BUTTON_A);          /* Cross    */
        h.run |= btn(SDL_CONTROLLER_BUTTON_X)            /* Square   */
              || axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.3f;
        h.start |= btn(SDL_CONTROLLER_BUTTON_START);
        h.select |= btn(SDL_CONTROLLER_BUTTON_BACK);
        h.tri |= btn(SDL_CONTROLLER_BUTTON_Y);           /* Triangle */
        h.ps |= btn(SDL_CONTROLLER_BUTTON_GUIDE);        /* PS       */
    }

    in->move = clampf(mv, -1, 1);
    in->jumpHeld = h.jump;
    in->runHeld = h.run;
    in->jumpP = h.jump && !G.prev.jump;
    in->startP = h.start && !G.prev.start;
    in->selectP = h.select && !G.prev.select;
    in->triP = h.tri && !G.prev.tri;
    in->psP = h.ps && !G.prev.ps;
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

static void fillS(float x, float y, float w, float h) { /* screen coords */
    SDL_FRect rc = { x, y, w, h };
    SDL_RenderFillRectF(G.ren, &rc);
}

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
    /* Sky gradient */
    for (int i = 0; i < 27; i++) {
        float t = i / 26.0f;
        color((Uint8)(80 + 110 * t), (Uint8)(150 + 75 * t), (Uint8)(225 + 25 * t), 255);
        fillS(0, i * 20.0f, VW, 21);
    }
    /* Sun */
    color(255, 240, 190, 255);
    fillS(760, 60, 60, 60);
    color(255, 250, 220, 255);
    fillS(770, 70, 40, 40);

    /* Clouds */
    color(250, 252, 255, 255);
    float cx0 = G.camX * 0.1f;
    for (int i = (int)floorf(cx0 / 280) - 1; i < (int)floorf((cx0 + VW) / 280) + 2; i++) {
        float x = i * 280 - cx0 + hashf(i * 7) * 120;
        float y = 40 + hashf(i * 13) * 120;
        float w = 70 + hashf(i * 3) * 60;
        fillS(x, y + 10, w, 18);
        fillS(x + 12, y, w * 0.55f, 14);
        fillS(x + w * 0.45f, y + 4, w * 0.35f, 10);
    }

    /* Far mountains (parallax 0.2) */
    color(130, 155, 205, 255);
    for (int sx = 0; sx < VW; sx += 3) {
        float h = ridge(sx + G.camX * 0.2f, 140, 140, 270, false, 11);
        fillS((float)sx, VH - h, 3, h);
    }
    /* Snow caps */
    color(235, 240, 250, 255);
    for (int sx = 0; sx < VW; sx += 3) {
        float h = ridge(sx + G.camX * 0.2f, 140, 140, 270, false, 11);
        if (h > 235) fillS((float)sx, VH - h, 3, (h - 235) * 0.8f);
    }
    /* Near hills (parallax 0.45) */
    color(95, 170, 120, 255);
    for (int sx = 0; sx < VW; sx += 3) {
        float h = ridge(sx + G.camX * 0.45f, 220, 70, 160, true, 97);
        fillS((float)sx, VH - h, 3, h);
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
        float wob = sinf(G.time * 12 + e->x) * 2;
        color(110, 45, 130, 255); fillW(e->x, e->y + 4, EW, EH - 4);
        color(160, 80, 190, 255); fillW(e->x + 2, e->y + 2, EW - 4, EH - 8);
        color(90, 35, 105, 255);
        fillW(e->x + 3, e->y + EH - 3 + wob * 0.5f, 7, 4);
        fillW(e->x + EW - 10, e->y + EH - 3 - wob * 0.5f, 7, 4);
        float d = e->vx > 0 ? 3 : -1;
        color(255, 255, 255, 255);
        fillW(e->x + 6 + d, e->y + 6, 6, 7); fillW(e->x + 15 + d, e->y + 6, 6, 7);
        color(30, 20, 40, 255);
        fillW(e->x + 8 + d, e->y + 8, 3, 4); fillW(e->x + 17 + d, e->y + 8, 3, 4);
    }

    /* Player */
    bool blink = G.invuln > 0 && fmodf(G.time, 0.16f) < 0.08f;
    if (G.st != ST_OVER && !blink) {
        float sq = G.squash * 0.25f;
        float w = PW * (1 + sq), h = PH * (1 - sq);
        float x = G.px - (w - PW) / 2, y = G.py + (PH - h);
        /* legs */
        float step = G.onGround && fabsf(G.vx) > 20 ? sinf(G.runAnim) * 4 : 0;
        color(40, 60, 120, 255);
        fillW(x + 3, y + h - 8 - fmaxf(0, step), 7, 8);
        fillW(x + w - 10, y + h - 8 - fmaxf(0, -step), 7, 8);
        /* body */
        color(220, 60, 60, 255); fillW(x, y + 10, w, h - 17);
        color(250, 110, 90, 255); fillW(x + 2, y + 11, w - 4, 3);
        /* head */
        color(250, 210, 170, 255); fillW(x + 2, y, w - 4, 12);
        color(60, 40, 30, 255); fillW(x + 1, y - 2, w - 2, 4); /* hair */
        float ex = G.facing > 0 ? x + w - 9 : x + 4;
        color(255, 255, 255, 255); fillW(ex, y + 3, 5, 5);
        color(20, 20, 40, 255); fillW(ex + (G.facing > 0 ? 2 : 0), y + 4, 3, 3);
        /* scarf */
        color(255, 205, 60, 255);
        fillW(G.facing > 0 ? x - 6 : x + w, y + 11 + sinf(G.time * 14) * 1.5f, 6, 3);
    }

    for (int i = 0; i < G.nParts; i++) {
        Part *p = &G.parts[i];
        color(p->r, p->g, p->b, (Uint8)(255 * clampf(p->life / p->max, 0, 1)));
        fillW(p->x, p->y, p->size, p->size);
    }
}

static void drawHUD(void) {
    char buf[96];
    snprintf(buf, sizeof buf, "SCORE %d", G.score);
    text(buf, 20, 16, 4, 255, 255, 255, false);
    snprintf(buf, sizeof buf, "COINS %d", G.coins);
    text(buf, 20, 44, 3, 255, 225, 90, false);
    snprintf(buf, sizeof buf, "BEST %d", G.best);
    text(buf, VW - 20 - textWidth(buf, 3), 16, 3, 230, 240, 255, false);
    for (int i = 0; i < START_LIVES; i++) {
        float x = VW - 20 - (START_LIVES - i) * 26.0f, y = 42;
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

static void drawControls(float y) {
    text("STICK / D-PAD  MOVE      CROSS  JUMP (HOLD = HIGHER)", VW / 2, y, 2, 230, 235, 255, true);
    text("SQUARE / R2  RUN      START  PAUSE      SELECT  RESTART", VW / 2, y + 22, 2, 230, 235, 255, true);
    text("TRIANGLE  FULLSCREEN      PS  QUIT", VW / 2, y + 44, 2, 230, 235, 255, true);
    text("KEYBOARD: ARROWS  SPACE  SHIFT  ENTER  R  F  Q", VW / 2, y + 76, 2, 170, 180, 210, true);
}

static void render(void) {
    shakeX = shakeY = 0;
    if (G.shake > 0) {
        shakeX = (frand() - 0.5f) * 14 * G.shake / 0.3f;
        shakeY = (frand() - 0.5f) * 14 * G.shake / 0.3f;
    }
    drawBackground();
    drawWorld();

    switch (G.st) {
    case ST_TITLE:
        overlay(140);
        text("DS3 JUMPER", VW / 2, 110, 12, 255, 220, 80, true);
        text("A TINY PLATFORMER FOR THE PS3 CONTROLLER", VW / 2, 200, 3, 255, 255, 255, true);
        if (fmodf(G.time, 1.0f) < 0.65f)
            text("PRESS CROSS OR SPACE TO START", VW / 2, 260, 4, 255, 255, 255, true);
        if (S.games > 0) {
            char buf[128];
            snprintf(buf, sizeof buf, "BEST %d   LAST %d   GAMES %d   COINS %d   STOMPS %d",
                     S.best, S.last, S.games, S.coinsTotal, S.stompsTotal);
            text(buf, VW / 2, 306, 2, 255, 225, 120, true);
        }
        drawControls(350);
        drawHUD();
        break;
    case ST_PLAY:
        drawHUD();
        break;
    case ST_PAUSE:
        drawHUD();
        overlay(150);
        text("PAUSED", VW / 2, 150, 10, 255, 255, 255, true);
        text("START: RESUME    SELECT: RESTART    PS: QUIT", VW / 2, 260, 3, 230, 235, 255, true);
        drawControls(350);
        break;
    case ST_OVER:
        drawHUD();
        overlay(160);
        text("GAME OVER", VW / 2, 140, 10, 255, 110, 100, true);
        {
            char buf[64];
            snprintf(buf, sizeof buf, "SCORE %d   BEST %d", G.score, G.best);
            text(buf, VW / 2, 240, 4, 255, 255, 255, true);
        }
        if (fmodf(G.time, 1.0f) < 0.65f)
            text("PRESS CROSS OR SPACE TO PLAY AGAIN", VW / 2, 300, 3, 255, 255, 255, true);
        break;
    }
}

/* ----------------------------------------------------------------- main */

static void setFullscreen(bool on) {
    G.fullscreen = on;
    SDL_SetWindowFullscreen(G.win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
}

/* Headless: simulate a few seconds of scripted play and save a BMP. */
static int screenshot(const char *path) {
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
    render();
    SDL_RenderPresent(G.ren);
    int rc = SDL_SaveBMP(surf, path);
    printf(rc == 0 ? "saved %s (score %d)\n" : "failed to save %s\n", path, G.score);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    bool windowed = false, stats = false;
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windowed") || !strcmp(argv[i], "-w")) windowed = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--stats")) stats = true;
        else {
            printf("usage: %s [--windowed] [--stats] [--screenshot out.bmp]\n", argv[0]);
            return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 2;
        }
    }

    initSavePath();
    loadSave();
    G.best = S.best;

    if (stats) {
        printf("save file: %s\n\n", savePath[0] ? savePath : "(unavailable)");
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
        int rc = screenshot(shot);
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

    G.win = SDL_CreateWindow("DS3 Jumper", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             VW, VH, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!G.win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    G.ren = SDL_CreateRenderer(G.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!G.ren) G.ren = SDL_CreateRenderer(G.win, -1, 0);
    if (!G.ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(G.ren, VW, VH);
    SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);
    if (!windowed) setFullscreen(true);

    for (int i = 0; i < SDL_NumJoysticks(); i++) openPad(i);

    newGame();
    G.st = ST_TITLE;

    Uint64 last = SDL_GetPerformanceCounter();
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
                    if (G.st == ST_PLAY) G.st = ST_PAUSE;
                }
                break;
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)SDL_GetPerformanceFrequency();
        last = now;
        if (dt > 0.05f) dt = 0.05f;

        readInput(&in);
        if (in.triP) setFullscreen(!G.fullscreen);

        switch (G.st) {
        case ST_TITLE:
            if (in.jumpP || in.startP) newGame();
            else if (in.psP) G.quit = true;
            break;
        case ST_PLAY:
            if (in.startP || in.psP) { G.st = ST_PAUSE; break; }
            if (in.selectP) { newGame(); break; }
            if (in.jumpP) G.jumpBuf = 0.12f;
            acc += dt;
            while (acc >= STEP && G.st == ST_PLAY) {
                update(STEP, &in);
                acc -= STEP;
            }
            break;
        case ST_PAUSE:
            acc = 0;
            if (in.startP) G.st = ST_PLAY;
            else if (in.selectP) newGame();
            else if (in.psP) G.quit = true;
            break;
        case ST_OVER:
            if (in.jumpP || in.startP || in.selectP) newGame();
            else if (in.psP) G.quit = true;
            break;
        }

        if (G.st != ST_PAUSE) updateParticles(dt);
        G.time += dt;
        G.shake = fmaxf(0, G.shake - dt);

        color(0, 0, 0, 255);
        SDL_RenderClear(G.ren);
        render();
        SDL_RenderPresent(G.ren);
    }

    endRun(); /* save a run that was in progress when quitting */
    if (G.pad) SDL_GameControllerClose(G.pad);
    SDL_DestroyRenderer(G.ren);
    SDL_DestroyWindow(G.win);
    SDL_Quit();
    return 0;
}
