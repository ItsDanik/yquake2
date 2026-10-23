// MiSTer hybrid core support, see mister_port.h

#define _GNU_SOURCE
#include "mister_port.h"

#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mister_hybrid.h>

#define EXIT_TO_MENU 0
#define EXIT_RESTART 42 // danik_hybrid_launch.sh starts the game again
#define EXIT_RELOADED 43 // the same, after the core has settled

static const char *const TITLE = "YQuake2";

// A field of the core's 15kHz modes: 400 x 262 pixels at 6.25MHz
// (hybrid/tools/fakecore.c)
#define FIELD_USEC 16768

// OSD options of the game (CONF_STR in core/YQuake2.sv, status bits 24..63).
// The first entry of each is 0, the default.

int MiSTer_GameHeight(void)
{
    static int fixed = -1;

    if (fixed < 0)
        fixed = getenv("MISTER_HYBRID_HEIGHT") ? atoi(getenv("MISTER_HYBRID_HEIGHT")) : 0;
    if (fixed == 200 || fixed == 240)
        return fixed;
    // "O[24],Resolution,320x200,320x240;"
    return MH_IsOpen() && MH_OSD_GAME_BITS(MH_OSDStatus(), 24, 1) ? 240 : 200;
}

// "O[30:29],Rumble,On,Strong,Off;"
float MiSTer_Rumble(void)
{
    static const float level[4] = { 1.0f, 2.0f, 0.0f, 0.0f };

    return MH_IsOpen() ? level[MH_OSD_GAME_BITS(MH_OSDStatus(), 29, 2)] : 0.0f;
}

// "D0D8P1O[86:82],HUD (Bottom)" (CRT Options), greyed out with mask bit 0
// while the game is in 320x200 (HudLines()). Quake II's margin is the bottom
// one (Danik, 2026-10-21): "D0D7P1O[81:77],HUD (Top)" is always greyed out
// (HUD_EDGES in core/YQuake2.sv) and the top row is the screen's edge.
// MISTER_HYBRID_HUD=<top>,<bottom> fixes them for a test (the library).
void MiSTer_HudRows(int height, int *top, int *bottom)
{
    int t, b;

    *top = 0;
    *bottom = height;
    if (height < 240 || !MH_IsOpen())
        return;
    MH_HudMargins(&t, &b);
    *bottom = height - b;
}

static void HudLines(void)
{
    static int mask = -1;
    int m = MiSTer_GameHeight() == 240 ? 0 : 1;

    if (m != mask)
        MH_SetMenuMask(m);
    mask = m;
}

// "P2O[25],God Mode", "P2O[26],No Clipping", "P2O[27],Invisible to Monsters",
// "P2O[28],Infinite Ammo" and the triggers "P2T[56],Give All" .. "P2T[59],Next
// Level" (core/YQuake2.sv). MISTER_HYBRID_CHEATS=<bits> fixes them for a
// test: the switches stay, the triggers are selected once a level runs.
int MiSTer_Cheats(int level)
{
    static int fixed = -2;
    int triggers;

    if (fixed == -2)
        fixed = getenv("MISTER_HYBRID_CHEATS") ? atoi(getenv("MISTER_HYBRID_CHEATS")) : -1;
    if (fixed >= 0) {
        if (!level)
            return fixed & MISTER_CHEAT_SWITCHES;
        triggers = fixed & ~MISTER_CHEAT_SWITCHES;
        fixed &= MISTER_CHEAT_SWITCHES;
        return fixed | triggers;
    }
    if (!MH_IsOpen())
        return 0;
    triggers = MH_OSDTriggers() & 15;
    return (int)MH_OSD_GAME_BITS(MH_OSDStatus(), 25, 4) | (level ? triggers << 4 : 0);
}

// Frames drawn a field ahead (hybrid/CLAUDE.md, "Frame pacing"): the frame
// for field F is started once the counter is F - 2 and handed over with
// MH_PresentAtField(F). Measured on the MiSTer (CLAUDE.md of the core).
#define DRAW_AHEAD 1

static uint32_t LastField; // with DRAW_AHEAD the field the last frame is shown at
static int FieldValid = 0;

// Development: see mister_port.h
static int LogEnabled = -1;
static int MarkNow;
static struct timespec MarkTime;
static double MarkSum[MISTER_T_COUNT];
static double MarkFrame[MISTER_T_COUNT]; // of the frame that is being made
static double MarkSlowest[MISTER_T_COUNT]; // of the frame that took longest
static double MarkSlowestWork;

#define PROFILE_MAX (1 << 20)
static uint32_t *ProfilePC;
static uint8_t *ProfileWhat;
static volatile unsigned ProfileCount;
static const char *ProfileFile;

static void ProfileSample(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    unsigned n = ProfileCount;

    (void)sig;
    (void)info;
    if (n < PROFILE_MAX) {
#ifdef __arm__
        ProfilePC[n] = uc->uc_mcontext.arm_pc;
#else
        ProfilePC[n] = (uint32_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
        ProfileWhat[n] = MarkNow;
        ProfileCount = n + 1;
    }
}

static void ProfileWrite(void)
{
    FILE *f = fopen(ProfileFile, "wb");
    FILE *maps = fopen("/proc/self/maps", "r");
    unsigned n = ProfileCount;
    char line[512];

    if (!f)
        return;
    // the memory map first, as text, for the game library
    while (maps && fgets(line, sizeof(line), maps))
        fputs(line, f);
    if (maps)
        fclose(maps);
    fprintf(f, "samples %u\n", n);
    fwrite(ProfilePC, 4, n, f);
    fwrite(ProfileWhat, 1, n, f);
    fclose(f);
}

static void ProfileStart(void)
{
    struct sigaction sa;
    struct sigevent ev;
    struct itimerspec its;
    clockid_t clock;
    timer_t timer;

    ProfileFile = getenv("MISTER_HYBRID_PROFILE");
    if (!ProfileFile)
        return;
    ProfilePC = malloc(PROFILE_MAX * 4);
    ProfileWhat = malloc(PROFILE_MAX);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ProfileSample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev._sigev_un._tid = syscall(SYS_gettid);
    if (pthread_getcpuclockid(pthread_self(), &clock) || timer_create(clock, &ev, &timer))
        return;
    its.it_interval.tv_sec = its.it_value.tv_sec = 0;
    its.it_interval.tv_nsec = its.it_value.tv_nsec = 1000000;
    timer_settime(timer, 0, &its, NULL);
    atexit(ProfileWrite);
}

void MiSTer_Mark(int what)
{
    struct timespec now;

    if (LogEnabled <= 0)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    MarkFrame[MarkNow] += (now.tv_sec - MarkTime.tv_sec) * 1e3 + (now.tv_nsec - MarkTime.tv_nsec) * 1e-6;
    MarkTime = now;
    MarkNow = what;
}

// Every 300 fields (5 seconds): the frames shown, how many of them came 1, 2,
// 3 and more fields after the one before, and the milliseconds per frame
static void FpsLog(unsigned fields)
{
    static const char *const names[MISTER_T_COUNT] = { "other", "wait", "input", "server", "client",
                                                       "render", "hud", "blit", "sound" };
    static unsigned frames, total, after[4];
    double work = 0;
    int i;

    if (LogEnabled < 0) {
        LogEnabled = getenv("MISTER_HYBRID_FPSLOG") && atoi(getenv("MISTER_HYBRID_FPSLOG"));
        if (LogEnabled) {
            clock_gettime(CLOCK_MONOTONIC, &MarkTime);
            ProfileStart();
        }
    }
    if (!LogEnabled || !fields)
        return;
    for (i = 0; i < MISTER_T_COUNT; i++) {
        MarkSum[i] += MarkFrame[i];
        if (i != MISTER_T_WAIT)
            work += MarkFrame[i];
    }
    if (work > MarkSlowestWork) {
        MarkSlowestWork = work;
        memcpy(MarkSlowest, MarkFrame, sizeof(MarkSlowest));
    }
    memset(MarkFrame, 0, sizeof(MarkFrame));
    frames++;
    total += fields;
    after[fields >= 4 ? 3 : fields - 1]++;
    if (total >= 300) {
        printf("mister: %u frames in %u fields (%.1f per second), after 1/2/3/more fields %u/%u/%u/%u, ms:",
               frames, total, frames * 1000000.0 / (total * FIELD_USEC), after[0], after[1], after[2], after[3]);
        for (i = 0; i < MISTER_T_COUNT; i++) {
            printf(" %s %.2f", names[i], MarkSum[i] / frames);
            MarkSum[i] = 0;
        }
        printf("\nmister: slowest frame %.1f ms:", MarkSlowestWork);
        for (i = 0; i < MISTER_T_COUNT; i++) {
            if (i != MISTER_T_WAIT && MarkSlowest[i] >= 0.5)
                printf(" %s %.1f", names[i], MarkSlowest[i]);
        }
        printf("\n");
        fflush(stdout);
        MarkSlowestWork = 0;
        frames = total = 0;
        memset(after, 0, sizeof(after));
    }
}

void MiSTer_ResetFrames(void)
{
    FieldValid = 0;
}

unsigned MiSTer_WaitFrame(void)
{
    uint32_t field;
    unsigned fields;

    if (!MH_IsOpen())
        return 0;
    if (!FieldValid) {
        LastField = MH_FieldCounter();
        FieldValid = 1;
    }
    MiSTer_Mark(MISTER_T_WAIT);
    HudLines();
    if (DRAW_AHEAD && !MH_Interlaced()) {
        uint32_t target = LastField + 1;

        MH_WaitField(target - 3); // not before the field two before its own
        field = MH_FieldCounter();
        if ((int32_t)(field + 1 - target) > 0)
            target = field + 1; // too late for it: the next vblank
        if (!MH_IsOpen())
            return 0;
        MH_PresentAtField(target);
        fields = target - LastField;
        LastField = target;
        MiSTer_Mark(MISTER_T_OTHER);
        FpsLog(fields);
        return fields;
    }
    MH_WaitFrame(LastField);
    field = MH_FieldCounter();
    fields = field - LastField; // 0: the core is gone
    LastField = field;
    MiSTer_Mark(MISTER_T_OTHER);
    FpsLog(fields);
    return fields;
}

int MiSTer_Paced(void)
{
    return MH_IsOpen() && FieldValid;
}

int MiSTer_FieldsToUsec(unsigned fields)
{
    return (int)(fields * FIELD_USEC);
}

// The player came through the list: quitting a game goes back to it
static int PickedFromList = 0;

void MiSTer_ShowError(const char *message)
{
    if (MH_Open())
        MH_UI_Message(TITLE, message, MH_UI_WAIT | MH_UI_ERROR);
}

// Quake II, its demo and the mission packs, each a menu entry
// (package/games.txt: the same ids and titles; the demo has none, Quake II's
// starts it), the set name YQuake2_<id>. Each is a folder next to the binary
// with its data and our build of its game library (game.so, which the
// package has: it says nothing). The full game and the demo both have
// baseq2/pak0.pak, the demo's 50MB, the full game's (CD, Steam, GOG) 180MB:
// told apart by its size (FullGame()), and the one that is not there gets
// MH_NOT_THERE. The mission packs need the full game. Messages: 4 lines of
// 71, %s the game's folder.
static const char *const BaseFiles[] = { "baseq2/pak0.pak", NULL };
static const char *const PackFiles[][3] = {
    { "baseq2/pak0.pak", "xatrix/pak0.pak", NULL },
    { "baseq2/pak0.pak", "rogue/pak0.pak", NULL },
};
#define DEMO_PAK_MAX (100 * 1024 * 1024)
static struct mh_game Games[] = {
    { "Q2", "Quake II", NULL, NULL,
      "Quake II was not found (baseq2/pak0.pak). Copy everything in the folder "
      "baseq2 of Quake II (CD, Steam or GOG, or the demo) into %s/baseq2/." },
    { "DEMO", "Quake II Demo", NULL, "Q2", NULL },
    { "RK", "Q2: The Reckoning", PackFiles[0], NULL,
      "The Reckoning was not found. It needs the full Quake II (its folder baseq2 "
      "with pak0.pak) and the mission pack's folder xatrix, both in %s/." },
    { "GZ", "Q2: Ground Zero", PackFiles[1], NULL,
      "Ground Zero was not found. It needs the full Quake II (its folder baseq2 "
      "with pak0.pak) and the mission pack's folder rogue, both in %s/." },
};
static const char *const GameDirs[] = { "baseq2", "baseq2", "xatrix", "rogue" };
#define NUM_GAMES ((int)(sizeof(Games) / sizeof(Games[0])))
#define NO_GAME "No game found (baseq2/pak0.pak). Copy everything in the folder baseq2 of " \
                "Quake II (CD, Steam or GOG, or the demo) into %s/baseq2/."

// baseq2/pak0.pak: 1 of the full game, 0 of the demo, -1 none
static int FullGame(void)
{
    struct stat st;

    if (stat("baseq2/pak0.pak", &st) != 0)
        return -1;
    return st.st_size > DEMO_PAK_MAX;
}

int MiSTer_SelectGame(int *argc, char ***argv)
{
    static char *args[64];
    int full, pick, i;

    if (!MH_Open())
        return 1;
    full = FullGame();
    Games[0].files = full == 1 ? BaseFiles : MH_NOT_THERE;
    Games[1].files = full == 0 ? BaseFiles : MH_NOT_THERE;
    for (i = 2; i < NUM_GAMES; i++) {
        if (full != 1)
            Games[i].files = MH_NOT_THERE;
    }
    // "Checking Game Data...", then the game's logo with "Loading <title>..."
    // come with the pick (MH_GameLogo(), host version 17); the demo has Quake
    // II's logo
    pick = MH_PickGame(Games, NUM_GAMES, NULL, NO_GAME, &PickedFromList);
    if (pick < 0)
        return 0;
    if (strcmp(GameDirs[pick], "baseq2") != 0 && *argc + 3 < (int)(sizeof(args) / sizeof(args[0]))) {
        memcpy(args, *argv, *argc * sizeof(char *));
        args[(*argc)++] = "+set";
        args[(*argc)++] = "game";
        args[(*argc)++] = (char *)GameDirs[pick];
        args[*argc] = NULL;
        *argv = args;
    }
    return 1;
}

int MiSTer_ExitCode(void)
{
    // The player loaded the core again while the game ran
    if (MH_CoreReloaded())
        return EXIT_RELOADED;
    // Not if we are quitting because another core was loaded
    if (PickedFromList && MH_Open())
        return EXIT_RESTART;
    return EXIT_TO_MENU;
}
