/*
 * The PC port's pause menu, for native builds only (the port compiles it as
 * an extra unit of the main image): Esc, or a controller's Guide button,
 * freezes the game and opens the game's own option dialogs over its last
 * picture, dimmed: Resume, Quit to title (the game's soft reset), Quit
 * game, each but Resume confirmed with the game's Yes and No.
 *
 * While it is open the main loop runs no game task: each frame runs only
 * the menu's own task list, draws its ordering table over the saved
 * picture and shows it (gamemain.c calls pcMenuFrame); it opens only where
 * the game loop draws the frames (gameplay, the title screen), not over
 * movies. The host asks for it and quits the game (the host's
 * pause_menu.rs).
 */
#ifdef PC_PORT

#include <psyq/sys/types.h>
#include <psyq/libgte.h>
#include <psyq/libgpu.h>

#include "common.h"
#include "main/display.h"
#include "main/display_types.h"
#include "main/fs.h"
#include "main/mem.h"
#include "main/pad.h"
#include "main/sound.h"
#include "main/sound_ids.h"
#include "main/task.h"
#include "main/text.h"
#include "main/ui.h"
#include "main/ui_types.h"
#include "../display.h"
#include "../fs.h"
#include "../mc.h"
#include "../task.h"

/* The host's side (pause_menu.rs). */
extern void pc_menu_available(s32 available);
extern s32 pc_menu_requested(void);
extern void pc_menu_taken(void);
extern s32 pc_menu_confirms_quit(void);
extern void pc_menu_quit(void);

/* ui.c: the shared option-dialog state, kept across the menu. */
extern void pcUiSaveState(void);
extern void pcUiRestoreState(void);

enum {
    PC_MENU_CLOSED,
    PC_MENU_PAUSE,
    PC_MENU_CONFIRM_TITLE,
    PC_MENU_CONFIRM_QUIT,
};

/* What happens once the menu's dialogs have closed. */
enum {
    PC_MENU_THEN_NOTHING,
    PC_MENU_THEN_PAUSE,
    PC_MENU_THEN_CONFIRM_TITLE,
    PC_MENU_THEN_CONFIRM_QUIT,
    PC_MENU_THEN_RESUME,
    PC_MENU_THEN_TITLE,
    PC_MENU_THEN_QUIT,
};

static u8 _gPcMenuPauseTitle[]    = "PAUSE";
static u8 _gPcMenuTitleTitle[]    = "QUIT TO TITLE";
static u8 _gPcMenuQuitTitle[]     = "QUIT GAME";
static u8 _gPcMenuResumeText[]    = "Resume";
static u8 _gPcMenuToTitleText[]   = "Quit to title";
static u8 _gPcMenuQuitGameText[]  = "Quit game";

static TaskNode              _gPcMenuTasks;
static UiOptionDialogRequest _gPcMenuRequest;
static UiDialogOption        _gPcMenuOptions[3];
static UiObject*             _gPcMenuDialog;
static s32                   _gPcMenuState;
static s32                   _gPcMenuThen;
/* What each row of the pause dialog does (its rows depend on the game's state). */
static s32 _gPcMenuPauseRows[3];
/* The game's last picture, shown dimmed behind the menu. */
static u16      _gPcMenuScreen[DISPLAY_SCREEN_WIDTH * DISPLAY_SCREEN_HEIGHT];
static TILE     _gPcMenuDimTile;
static DR_TPAGE _gPcMenuDimMode;
/* Quit to title asked, for gamemain.c's soft reset at the top of its loop. */
static s32 _gPcMenuTitleRequested;

/// Whether quitting to the title is possible: what the game's soft reset
/// needs (gamemain.c).
static s32 _pcMenuCanQuitToTitle(void)
{
    return gDisplayState.gameMode == DISPLAY_GAME_ACTIVE && gDisplayState.gameRunning != 0;
}

/// Widens `panel` (kept centred) to hold its title: the game's dialogs fit
/// their options only, its titles being short. The title is drawn in the
/// small face from three pixels in, on a tab a little wider.
static void _pcMenuFitTitle(UiPanel* panel, const u8* title)
{
    enum { PC_MENU_TITLE_MARGIN_PIXELS = 16 };
    TextDrawReq request;
    s32         wanted;

    request.glyphTable = TEXT_GLYPH_TABLE_SMALL;
    request.alignment  = TEXT_ALIGNMENT_RIGHT;
    request.x          = 0;
    request.y          = 0;
    textAlignLine(&request, title);
    wanted = -request.x + PC_MENU_TITLE_MARGIN_PIXELS;
    if ((s16)panel->bounds.unsignedRect.w < wanted) {
        panel->bounds.unsignedRect.w = wanted;
        panel->bounds.unsignedRect.x = -(wanted / 2);
    }
}

/// Opens an option dialog on the menu's task list.
static void _pcMenuOpenDialog(u8* title, u8** labels, s32 count)
{
    TaskNode* previous;
    s32       i;

    for (i = 0; i < count; i++) {
        _gPcMenuOptions[i].text = labels[i];
        _gPcMenuOptions[i].next = i + 1 < count ? &_gPcMenuOptions[i + 1] : NULL;
    }
    _gPcMenuRequest.optionCount = count;
    _gPcMenuRequest.options     = _gPcMenuOptions;
    _gPcMenuRequest.title       = (char*)title;
    _gPcMenuRequest.flags       = USER_INTERFACE_OPTION_DIALOG_CANCELLABLE;
    previous                    = taskGetActiveList();
    taskSetActiveList(&_gPcMenuTasks);
    _gPcMenuDialog = uiSpawnOptionDialog(&_gPcMenuRequest, 0, 0, 0);
    taskSetActiveList(previous);
    if (_gPcMenuDialog != NULL) {
        _pcMenuFitTitle(&_gPcMenuDialog->panel, title);
    }
}

static void _pcMenuOpenPause(void)
{
    u8* labels[3];
    s32 count;

    count                       = 0;
    _gPcMenuPauseRows[count]    = PC_MENU_THEN_RESUME;
    labels[count++]             = _gPcMenuResumeText;
    if (_pcMenuCanQuitToTitle()) {
        _gPcMenuPauseRows[count] = PC_MENU_THEN_CONFIRM_TITLE;
        labels[count++]          = _gPcMenuToTitleText;
    }
    _gPcMenuPauseRows[count] = PC_MENU_THEN_CONFIRM_QUIT;
    labels[count++]          = _gPcMenuQuitGameText;
    _pcMenuOpenDialog(_gPcMenuPauseTitle, labels, count);
    _gPcMenuState = PC_MENU_PAUSE;
}

static void _pcMenuOpenConfirm(s32 state)
{
    u8* labels[2];

    // The game's own Yes and No (main/mc.c), No first as the safe choice.
    labels[0] = gMcTextNo;
    labels[1] = gMcTextYes;
    _pcMenuOpenDialog(state == PC_MENU_CONFIRM_TITLE ? _gPcMenuTitleTitle : _gPcMenuQuitTitle, labels, 2);
    _gPcMenuState = state;
}

/// Whether the game's frames are ones the menu's can stand in for: the game
/// loop's own, at 320x240.
static s32 _pcMenuAvailable(void)
{
    return gDisplayState.displayOwner == DISPLAY_OWNER_GAME_LOOP && gDisplayState.height == DISPLAY_SCREEN_HEIGHT;
}

/// Whether the menu may open now: available, with nothing holding the
/// screen or streaming a scene.
static s32 _pcMenuCanOpen(s32 blocked)
{
    return !blocked && _pcMenuAvailable() && gCdCmdQueue.paceToSceneTiming == 0;
}

static void _pcMenuOpen(void)
{
    RECT area;

    // The last picture drawn, kept to be drawn again behind the menu.
    DrawSync(0);
    area   = gDisplayState.drawEnv[gDisplayState.otBuffer].clip;
    area.w = DISPLAY_SCREEN_WIDTH;
    area.h = DISPLAY_SCREEN_HEIGHT;
    StoreImage(&area, (u_long*)_gPcMenuScreen);
    DrawSync(0);

    _gPcMenuTasks.next = NULL;
    _gPcMenuTasks.prev = &_gPcMenuTasks;
    pcUiSaveState();
    sndEvtRequestScriptDuckAcquire();
    _gPcMenuThen = PC_MENU_THEN_NOTHING;
    _pcMenuOpenPause();
}

/// After a dialog's answer: what to do once it has closed.
static void _pcMenuAnswer(s32 result)
{
    switch (_gPcMenuState) {
        case PC_MENU_PAUSE:
            _gPcMenuThen = result > 0 ? _gPcMenuPauseRows[result - 1] : PC_MENU_THEN_RESUME;
            break;
        case PC_MENU_CONFIRM_TITLE:
            _gPcMenuThen = result == 2 ? PC_MENU_THEN_TITLE : PC_MENU_THEN_PAUSE;
            break;
        case PC_MENU_CONFIRM_QUIT:
            _gPcMenuThen = result == 2 ? PC_MENU_THEN_QUIT : PC_MENU_THEN_PAUSE;
            break;
    }
    _gPcMenuDialog = NULL;
}

/// Once the dialogs have closed: the next one, or back to the game.
static void _pcMenuAfterClosing(void)
{
    s32 then;

    then         = _gPcMenuThen;
    _gPcMenuThen = PC_MENU_THEN_NOTHING;
    switch (then) {
        case PC_MENU_THEN_PAUSE:
            _pcMenuOpenPause();
            return;
        case PC_MENU_THEN_CONFIRM_TITLE:
            _pcMenuOpenConfirm(PC_MENU_CONFIRM_TITLE);
            return;
        case PC_MENU_THEN_CONFIRM_QUIT:
            if (pc_menu_confirms_quit()) {
                _pcMenuOpenConfirm(PC_MENU_CONFIRM_QUIT);
                return;
            }
            pc_menu_quit();
            return;
        case PC_MENU_THEN_QUIT:
            pc_menu_quit();
            return;
        case PC_MENU_THEN_TITLE:
            _gPcMenuTitleRequested = 1;
            break;
    }
    // Back to the game: its dialogs as they were, its sound, and no button
    // the menu took reaching it.
    pcUiRestoreState();
    sndEvtRequestScriptDuckRelease();
    padStartInputBlock(0);
    _gPcMenuState = PC_MENU_CLOSED;
}

/// Draws the menu's frame: the saved picture, dimmed, under its ordering
/// table, into buffer `bufferIndex`, then shows it.
static void _pcMenuPresent(s32 bufferIndex)
{
    RECT area;
    s32  i;

    area   = gDisplayState.drawEnv[bufferIndex].clip;
    area.w = DISPLAY_SCREEN_WIDTH;
    area.h = DISPLAY_SCREEN_HEIGHT;
    PutDrawEnv(&gDisplayState.drawEnv[bufferIndex]);
    LoadImage(&area, (u_long*)_gPcMenuScreen);
    DrawSync(0);

    // Halved, as the game's own pause screen dims (gamemain.c).
    setDrawTPage(&_gPcMenuDimMode, true, true, getTPage(0, GPU_BLEND_AVERAGE, 0, 0));
    DrawPrim(&_gPcMenuDimMode);
    setTile(&_gPcMenuDimTile);
    setRGB0(&_gPcMenuDimTile, 0, 0, 0);
    setSemiTrans(&_gPcMenuDimTile, true);
    _gPcMenuDimTile.x0 = -DISPLAY_SCREEN_WIDTH / 2;
    _gPcMenuDimTile.y0 = -DISPLAY_SCREEN_HEIGHT / 2 - gDisplayState.vramYOffset;
    _gPcMenuDimTile.w  = DISPLAY_SCREEN_WIDTH;
    _gPcMenuDimTile.h  = DISPLAY_SCREEN_HEIGHT;
    DrawPrim(&_gPcMenuDimTile);

    DrawOTag((u_long*)gGpuFrameOrderingTables[bufferIndex].tag);
    DrawSync(0);
    VSync(0);
    // The display area showing what was just drawn.
    for (i = 0; i < 2; i++) {
        if (gDisplayState.dispEnv[i].disp.y == area.y && gDisplayState.dispEnv[i].disp.x == area.x) {
            PutDispEnv(&gDisplayState.dispEnv[i]);
        }
    }
}

s32 pcMenuFrame(s32 blocked)
{
    TaskNode* previous;
    s32       bufferIndex;

    // Whether the game draws its own frames, where the menu can open (a
    // moment's block aside): elsewhere the host asks to quit at once.
    pc_menu_available(_pcMenuAvailable());
    if (_gPcMenuState == PC_MENU_CLOSED) {
        if (!pc_menu_requested() || !_pcMenuCanOpen(blocked)) {
            return 0;
        }
        pc_menu_taken();
        _pcMenuOpen();
    } else if (pc_menu_requested()) {
        // Esc again: as the dialog's Cancel.
        pc_menu_taken();
        if (_gPcMenuDialog != NULL) {
            uiStartTreeClosing(_gPcMenuDialog, NULL);
            _pcMenuAnswer(-1);
        }
    }

    // A frame of the menu's own: the buffers' other half, as the game's
    // frames alternate, and none of the game's clocks.
    bufferIndex              = gDisplayState.otBuffer ^ 1;
    gDisplayState.otBuffer   = bufferIndex;
    gDisplayState.drawBuffer = bufferIndex;
    gDisplayState.animFrame++;
    _gpuBeginOt(bufferIndex);
    gGpuSystemPrimCursor = gDisplayStaticPrimitiveArena + bufferIndex * (s32)(sizeof(gDisplayStaticPrimitiveArena) / 2);
    gGpuPrimCursor       = gGpuPrimitiveBufferBase + bufferIndex * (gGpuPrimitiveBufferBytes >> 1);

    previous = taskGetActiveList();
    taskExecList(&_gPcMenuTasks);
    taskSetActiveList(previous);

    if (_gPcMenuDialog != NULL && _gPcMenuRequest.result != 0) {
        _pcMenuAnswer(_gPcMenuRequest.result);
    }
    if (_gPcMenuDialog == NULL && _gPcMenuTasks.next == NULL) {
        _pcMenuAfterClosing();
    }

    cdCmdService();
    _pcMenuPresent(bufferIndex);
    return 1;
}

/// The menu's own texts (not the game's, so not on the disc), for the host's
/// extract to list for translators (mods' strings/ translate them as the
/// game's); `count` gets how many.
const u8* const* pcMenuTexts(s32* count)
{
    static const u8* const texts[] = {
        _gPcMenuPauseTitle, _gPcMenuResumeText, _gPcMenuToTitleText, _gPcMenuQuitGameText, _gPcMenuTitleTitle, _gPcMenuQuitTitle,
    };

    *count = ARRAY_SIZE(texts);
    return texts;
}

s32 pcMenuTakeTitleRequest(void)
{
    s32 requested;

    requested              = _gPcMenuTitleRequested;
    _gPcMenuTitleRequested = 0;
    return requested;
}

#endif
