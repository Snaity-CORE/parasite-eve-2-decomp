/*
 * The PC port's pause menu, for native builds only (the port compiles it as
 * an extra unit of the main image): Esc, or a controller's Guide button,
 * freezes the game and opens the game's own option dialogs over its last
 * picture, dimmed: Resume, Options (the host's settings, in a panel of the
 * game's options' look for each category), Quit to title (the game's soft
 * reset), Quit game, the last two confirmed with the game's Yes and No.
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

/* The host's settings (settings.rs): categories, their rows, each row's
 * label and choices (NULL past the last; -1 the current one). */
extern s32       pc_options_categories(void);
extern const u8* pc_options_category(s32 category, s32 title);
extern s32       pc_options_count(s32 category);
extern const u8* pc_options_label(s32 category, s32 row);
extern const u8* pc_options_choice(s32 category, s32 row, s32 choice);
extern s32       pc_options_needs_restart(s32 category, s32 row, s32 ever);
extern const u8* pc_options_restart_text(void);
extern void      pc_options_change(s32 category, s32 row, s32 delta);
extern void      pc_options_save(void);

/* ui.c: the shared option-dialog state, kept across the menu. */
extern void pcUiSaveState(void);
extern void pcUiRestoreState(void);
extern void pcUiSelectDialogRow(s32 row);

enum {
    PC_MENU_CLOSED,
    PC_MENU_PAUSE,
    PC_MENU_CONFIRM_TITLE,
    PC_MENU_CONFIRM_QUIT,
    PC_MENU_CATEGORIES,
    PC_MENU_SETTINGS,
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
    PC_MENU_THEN_CATEGORIES,
    PC_MENU_THEN_SETTINGS,
};

static u8 _gPcMenuPauseTitle[]    = "PAUSE";
static u8 _gPcMenuTitleTitle[]    = "QUIT TO TITLE";
static u8 _gPcMenuQuitTitle[]     = "QUIT GAME";
static u8 _gPcMenuResumeText[]    = "Resume";
static u8 _gPcMenuOptionsText[]   = "Options";
static u8 _gPcMenuOptionsTitle[]  = "OPTIONS";
static u8 _gPcMenuToTitleText[]   = "Quit to title";
static u8 _gPcMenuQuitGameText[]  = "Quit game";

static TaskNode              _gPcMenuTasks;
static UiOptionDialogRequest _gPcMenuRequest;
static UiDialogOption        _gPcMenuOptions[8];
static UiObject*             _gPcMenuDialog;
static s32                   _gPcMenuState;
static s32                   _gPcMenuThen;
/* What each row of the pause dialog does (its rows depend on the game's state). */
static s32 _gPcMenuPauseRows[4];
/* The settings' category shown, its panel and its row list. */
static s32       _gPcMenuCategory;
// The menus' font has no < or >: its guillemets point instead.
static u8        _gPcMenuCaretLeft[]  = "\xAB";
static u8        _gPcMenuCaretRight[] = "\xBB";
// A caret and the gap after it, in pixels; measured with the panel.
static s32       _gPcMenuCaretWidth;
enum { PC_MENU_CARET_GAP_PIXELS = 3 };
static UiObject* _gPcMenuPanel;
static void      _pcMenuDrawSettingRow(UiList* list, UiObject* object);
static UiListRowCallback _gPcMenuSettingRowCallbacks[] = { _pcMenuDrawSettingRow };
static UiList    _gPcMenuSettingList;
/* Where a row's choice is drawn, and each row's widest choice (content pixels). */
static s32 _gPcMenuChoiceX;
static s32 _gPcMenuRowChoiceWidth[16];
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

/// Opens an option dialog on the menu's task list, its cursor on `selected`.
static void _pcMenuOpenDialog(u8* title, u8** labels, s32 count, s32 selected)
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
    pcUiSelectDialogRow(selected);
    if (_gPcMenuDialog != NULL) {
        _pcMenuFitTitle(&_gPcMenuDialog->panel, title);
    }
}

static void _pcMenuOpenPause(void)
{
    u8* labels[4];
    s32 count;

    count                       = 0;
    _gPcMenuPauseRows[count]    = PC_MENU_THEN_RESUME;
    labels[count++]             = _gPcMenuResumeText;
    _gPcMenuPauseRows[count]    = PC_MENU_THEN_CATEGORIES;
    labels[count++]             = _gPcMenuOptionsText;
    if (_pcMenuCanQuitToTitle()) {
        _gPcMenuPauseRows[count] = PC_MENU_THEN_CONFIRM_TITLE;
        labels[count++]          = _gPcMenuToTitleText;
    }
    _gPcMenuPauseRows[count] = PC_MENU_THEN_CONFIRM_QUIT;
    labels[count++]          = _gPcMenuQuitGameText;
    _pcMenuOpenDialog(_gPcMenuPauseTitle, labels, count, 0);
    _gPcMenuState = PC_MENU_PAUSE;
}

static void _pcMenuOpenCategories(void)
{
    u8* labels[8];
    s32 count;
    s32 i;

    count = pc_options_categories();
    if (count > ARRAY_SIZE(labels)) {
        count = ARRAY_SIZE(labels);
    }
    for (i = 0; i < count; i++) {
        labels[i] = (u8*)pc_options_category(i, 0);
    }
    // Back from a category, on that category.
    _pcMenuOpenDialog(_gPcMenuOptionsTitle, labels, count, _gPcMenuCategory < count ? _gPcMenuCategory : 0);
    _gPcMenuState = PC_MENU_CATEGORIES;
}

/// Draws a settings row: its label, then its choice between carets on the
/// selected row, "(restart)" beside a change that waits for one; Left and
/// Right change it. Laid out as the game's options rows are (options.c).
static void _pcMenuDrawSettingRow(UiList* list, UiObject* object)
{
    s32       row;
    s32       x;
    s32       y;
    u32       dim;
    const u8* choice;

    row = list->currentItemIndex;
    y   = list->rowTextY.signedValue;
    x   = object->panel.contentLeft.signedValue;
    dim = uiGetTextColor(object, USER_INTERFACE_TEXT_COLOR_DIMMED);
    textDrawUiLine(object, x + 6, y, pc_options_label(_gPcMenuCategory, row), list->colorRgb, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    x += _gPcMenuChoiceX;
    choice = pc_options_choice(_gPcMenuCategory, row, -1);
    if (list->selectedItemIndex == row) {
        textDrawUiLine(object, x, y, _gPcMenuCaretLeft, dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
        textDrawUiLine(object, x + _gPcMenuCaretWidth + textMeasureLineWidth(choice) + PC_MENU_CARET_GAP_PIXELS, y,
                       _gPcMenuCaretRight, dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    }
    textDrawUiLine(object, x + _gPcMenuCaretWidth, y, choice, uiGetTextColor(object, USER_INTERFACE_TEXT_COLOR_NORMAL),
                   TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    if (pc_options_needs_restart(_gPcMenuCategory, row, 0)) {
        textDrawUiLine(object, x + 2 * _gPcMenuCaretWidth + _gPcMenuRowChoiceWidth[row] + 4, y, pc_options_restart_text(),
                       dim, TEXT_DRAW_OUTLINED, TEXT_ALIGNMENT_LEFT);
    }
    if (list->rowInputEnabled == USER_INTERFACE_LIST_ROW_ACTIVE) {
        if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, PAD_BUTTON_RIGHT) != 0) {
            sndEvtRequestScriptStart(SOUND_MENU_CURSOR, 0, 0);
            pc_options_change(_gPcMenuCategory, row, 1);
        } else if (padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, PAD_BUTTON_LEFT) != 0) {
            sndEvtRequestScriptStart(SOUND_MENU_CURSOR, 0, 0);
            pc_options_change(_gPcMenuCategory, row, -1);
        }
    }
}

/// The settings panel's task: fitted to its rows on its first update, then
/// its title and rows each frame; Cancel publishes a confirm for the menu
/// to close it (as the game's options panel does).
static void _pcMenuUpdateSettingsTask(Task* owningTask)
{
    enum { PC_MENU_ROW_HEIGHT_PIXELS = 16 };
    UiObject*  object;
    UiList*    list;
    const u8*  text;
    s32        rows;
    s32        row;
    s32        choice;
    s32        width;
    s32        labelWidth;
    s32        restartWidth;
    s32        rowWidth;

    object = owningTask->spawnArg2.pointer;
    list   = &_gPcMenuSettingList;
    if (owningTask->state == 0) {
        rows                            = pc_options_count(_gPcMenuCategory);
        list->rowCallbacks              = _gPcMenuSettingRowCallbacks;
        list->itemCount                 = rows;
        list->visibleRowCount.unsignedValue = rows;
        list->wrapNavigation            = 0;
        list->rowHeight                 = PC_MENU_ROW_HEIGHT_PIXELS;
        list->selectedItemIndex         = 0;
        uiFitPanelToList(list, &object->panel);
        list->flags = USER_INTERFACE_LIST_SHARED_ROW_CALLBACK;
        // Columns: the widest label, then each row's widest choice, with
        // "(restart)" after it on a row that may need one.
        if (rows > ARRAY_SIZE(_gPcMenuRowChoiceWidth)) {
            rows = ARRAY_SIZE(_gPcMenuRowChoiceWidth);
        }
        restartWidth       = textMeasureLineWidth(pc_options_restart_text());
        _gPcMenuCaretWidth = textMeasureLineWidth(_gPcMenuCaretLeft) + PC_MENU_CARET_GAP_PIXELS;
        labelWidth         = 0;
        rowWidth           = 0;
        for (row = 0; row < rows; row++) {
            width = textMeasureLineWidth(pc_options_label(_gPcMenuCategory, row));
            if (labelWidth < width) {
                labelWidth = width;
            }
            _gPcMenuRowChoiceWidth[row] = 0;
            for (choice = 0; (text = pc_options_choice(_gPcMenuCategory, row, choice)) != NULL; choice++) {
                width = textMeasureLineWidth(text);
                if (_gPcMenuRowChoiceWidth[row] < width) {
                    _gPcMenuRowChoiceWidth[row] = width;
                }
            }
            width = 2 * _gPcMenuCaretWidth + _gPcMenuRowChoiceWidth[row];
            if (pc_options_needs_restart(_gPcMenuCategory, row, 1)) {
                width += 4 + restartWidth;
            }
            if (rowWidth < width) {
                rowWidth = width;
            }
        }
        _gPcMenuChoiceX = 6 + labelWidth + 16;
        uiSetPanelContentSize(&object->panel, _gPcMenuChoiceX + rowWidth + 6, 0);
        object->panel.bounds.rect.x = -(object->panel.bounds.rect.w / 2);
        object->panel.bounds.rect.y = -(object->panel.bounds.rect.h / 2);
        _pcMenuFitTitle(&object->panel, pc_options_category(_gPcMenuCategory, 1));
        owningTask->state = 1;
    }
    object->result = USER_INTERFACE_RESULT_NONE;
    uiDrawPanelLabel(&object->panel, (const char*)pc_options_category(_gPcMenuCategory, 1));
    uiUpdateList(list, &object->panel);
    if (object->panel.control.word == USER_INTERFACE_PANEL_ACTIVE &&
        padCheckButtons(0, PAD_BUTTON_QUERY_PRESSED, gPadCancelButtonMask) != 0) {
        sndEvtRequestScriptStart(SOUND_MENU_CANCEL, 0, 0);
        object->result = USER_INTERFACE_RESULT_CONFIRM;
    }
}

/// The settings panel's recipe: a titled panel, sized on its first update.
static UiObjectDesc _gPcMenuSettingsDesc = {
    USER_INTERFACE_PANEL_TITLE_STYLE, { -100, -60, 200, 120 }, 32, 0, TASK_BODY_NONE, TASK_PRIORITY_CONTROL, _pcMenuUpdateSettingsTask, 0
};

static void _pcMenuOpenSettings(void)
{
    TaskNode*    previous;
    TaskSpawnArg none;

    none.value = 0;
    previous   = taskGetActiveList();
    taskSetActiveList(&_gPcMenuTasks);
    _gPcMenuPanel = uiSpawnObject(&_gPcMenuSettingsDesc, none, USER_INTERFACE_PANEL_ACTIVE, 1, NULL);
    taskSetActiveList(previous);
    _gPcMenuState = PC_MENU_SETTINGS;
}

static void _pcMenuOpenConfirm(s32 state)
{
    u8* labels[2];

    // The game's own Yes and No (main/mc.c), No first as the safe choice.
    labels[0] = gMcTextNo;
    labels[1] = gMcTextYes;
    _pcMenuOpenDialog(state == PC_MENU_CONFIRM_TITLE ? _gPcMenuTitleTitle : _gPcMenuQuitTitle, labels, 2, 0);
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
    _gPcMenuCategory = 0;
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
        case PC_MENU_CATEGORIES:
            if (result > 0) {
                _gPcMenuCategory = result - 1;
                _gPcMenuThen     = PC_MENU_THEN_SETTINGS;
            } else {
                _gPcMenuThen = PC_MENU_THEN_PAUSE;
            }
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
        case PC_MENU_THEN_CATEGORIES:
            _pcMenuOpenCategories();
            return;
        case PC_MENU_THEN_SETTINGS:
            _pcMenuOpenSettings();
            if (_gPcMenuPanel != NULL) {
                return;
            }
            break;
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
        } else if (_gPcMenuPanel != NULL) {
            _gPcMenuPanel->result = USER_INTERFACE_RESULT_CONFIRM;
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
    // The settings panel closed: its settings written, back to the categories.
    if (_gPcMenuPanel != NULL && _gPcMenuPanel->result == USER_INTERFACE_RESULT_CONFIRM) {
        pc_options_save();
        uiStartTreeClosing(_gPcMenuPanel, NULL);
        _gPcMenuPanel = NULL;
        _gPcMenuThen  = PC_MENU_THEN_CATEGORIES;
    }
    if (_gPcMenuDialog == NULL && _gPcMenuPanel == NULL && _gPcMenuTasks.next == NULL) {
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
        _gPcMenuPauseTitle, _gPcMenuResumeText, _gPcMenuOptionsText, _gPcMenuToTitleText, _gPcMenuQuitGameText, _gPcMenuTitleTitle, _gPcMenuQuitTitle,
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
