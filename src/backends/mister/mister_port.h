#ifndef MISTER_PORT_H
#define MISTER_PORT_H

// MiSTer hybrid core: the game runs on the ARM, the FPGA core shows the
// picture at 15kHz and provides audio and input. SDL's "mister" drivers
// (hybrid/sdl2) do most of the work; this is what is specific to the game.
// Everything is a no-op without the core, so the PC build still runs.

#ifdef __cplusplus
extern "C" {
#endif

// Joystick buttons as SDL sees them: the buttons of the core's "J1," list
// (core/YQuake2.sv) in order, then the two the SDL driver adds. The game
// takes the joystick as an SDL game controller (MiSTer_AddMappings() in
// client/input/sdl2.c) and binds its buttons (Key_MiSTerLayout()).
enum {
    MISTER_JOY_JUMP,      // A
    MISTER_JOY_CROUCH,    // B
    MISTER_JOY_INVENTORY, // X
    MISTER_JOY_USE_ITEM,  // Y
    MISTER_JOY_WALK,      // L
    MISTER_JOY_ATTACK,    // R
    MISTER_JOY_HELP,      // Select
    MISTER_JOY_MENU,      // Start
    MISTER_JOY_CORE_OK,
    MISTER_JOY_CORE_BACK,

    MISTER_JOY_MENU_OK = 28,
    MISTER_JOY_MENU_BACK = 29,
    MISTER_JOY_L3 = 30,   // the stick clicks, read from Linux (MH_ReadStickButtons())
    MISTER_JOY_R3 = 31
};

// Frame pacing: call once per frame in place of the game's own wait. Sleeps
// until the core has taken the last frame and returns how many fields
// (1/59.64s) have passed since the last call, which is the game's clock. 0
// without the core: use the original timing then.
unsigned MiSTer_WaitFrame(void);
// The next call starts counting anew, after the game stood still (loading)
void MiSTer_ResetFrames(void);
// Game time of that many fields, in microseconds
int MiSTer_FieldsToUsec(unsigned fields);
// 1 while the core paces the game
int MiSTer_Paced(void);

// Development, MISTER_HYBRID_FPSLOG=1: every 300 fields, the frames shown and
// where the time of a frame went. The engine says what it starts doing
// (MISTER_MARK() in common.h); the log has the milliseconds per frame of each.
// MISTER_HYBRID_PROFILE=<file> samples the program counter of the game thread
// as well, 1000 times per second of its CPU time, and writes the samples there
// on leaving (tools/profile.py reads them).
enum {
    MISTER_T_OTHER,
    MISTER_T_WAIT,   // for the core to take the frame
    MISTER_T_INPUT,  // IN_Update()
    MISTER_T_SERVER, // SV_Frame(): the game's logic
    MISTER_T_CLIENT, // the rest of CL_Frame()
    MISTER_T_RENDER, // the 3D view
    MISTER_T_HUD,    // 2D: status bar, console, menus
    MISTER_T_BLIT,   // the frame to the core
    MISTER_T_SOUND,  // S_Update()
    MISTER_T_COUNT
};
void MiSTer_Mark(int what);

// OSD "Resolution": the height of the game's screen, 200 or 240 (320 wide).
// MISTER_HYBRID_HEIGHT=200|240 fixes it for a test.
int MiSTer_GameHeight(void);
// The HUD margins of the OSD's CRT Options for a screen of `height` rows: the
// row the top HUD starts at and the one the bottom HUD ends at. 0 and
// `height` in 200 rows (the HUD lines are greyed out then). Read every frame.
void MiSTer_HudRows(int height, int *top, int *bottom);
// The OSD's Cheats page, once a frame: the MISTER_CHEAT_* switches that are
// on, and the triggers selected since the last call (MH_OSDTriggers(): from
// one place only). `level`: 0 while no level of a single player game runs,
// and the triggers are dropped
#define MISTER_CHEAT_GOD      1   // God Mode
#define MISTER_CHEAT_NOCLIP   2   // No Clipping
#define MISTER_CHEAT_NOTARGET 4   // Invisible to Monsters
#define MISTER_CHEAT_AMMO     8   // Infinite Ammo
#define MISTER_CHEAT_SWITCHES 15
#define MISTER_CHEAT_GIVE     16  // the trigger Give All
#define MISTER_CHEAT_QUAD     32  // the trigger Quad Damage
#define MISTER_CHEAT_INVULN   64  // the trigger Invulnerability
#define MISTER_CHEAT_NEXT     128 // the trigger Next Level
int MiSTer_Cheats(int level);

// Before the engine starts: the game of the menu entry the core was loaded
// from (MISTER_HYBRID_SET, YQuake2_<id>), or the one installed, or the one the
// player picks from the list of those (MH_PickGame()); then "Loading
// <title>..." until the first frame. Adds "+set game <dir>" to the arguments
// for a mission pack. Returns 0 if there is nothing to play (said on the
// screen until the player left the core) or the player backed out of the list.
int MiSTer_SelectGame(int *argc, char ***argv);
// Problems the player can fix (missing data): shown on the screen until a
// button is pressed, in plain words that say which files go where
void MiSTer_ShowError(const char *message);
// Exit code of the process after the player quit: the launcher starts the
// game again (back to the list of MiSTer_SelectGame) or returns to the menu
int MiSTer_ExitCode(void);

#ifdef __cplusplus
}
#endif

#endif
