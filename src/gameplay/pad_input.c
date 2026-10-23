#include "pad_input.h"

#include "common.h"

#include "gameplay/area_entry.h"
#include "gameplay/direction_input.h"
#include "pad_script.h"

#include "main/display.h"
#include "main/mc.h"
#include "main/pad.h"
#include "main/session.h"
#include "main/wipsys.h"

#ifdef PC_PORT
#include <psyq/sys/types.h>
#include <psyq/libgte.h>

#include "main/gfx.h"
#endif

/// Last automatic menu-lock decision (0 unlocked, 1 locked), frozen by explicit hold.
static u8 _gPadInputMenuLocked;

/// Automatic decision last applied to the logical Select/Start suppression bits.
static u8 _gPadInputPreviousMenuLocked;

/// Logical button bits removed after layout remapping, before session publication.
///
/// Explicit requests and automatic Select/Start locking share this 16-bit
/// latch without reference counting. Suppression edges change this module's
/// display holds on player-present updates; resident samples remain available.
static u16 _gPadInputSuppressionMask;

/// Logical-button suppression mask sampled by the preceding player-present update.
static u16 _gPadInputPreviousSuppressionMask;

/// Logical suppression bits newly set by this update, truncated to sixteen bits.
static u16 _gPadInputSuppressionSet;

/// Logical suppression bits newly cleared by this update, truncated to sixteen bits.
static u16 _gPadInputSuppressionCleared;

/// Outstanding display menu holds acquired by gameplay's suppression edges.
///
/// Outside demos, any newly suppressed Select/Start bit adds one hold; any
/// cleared Select/Start bit releases all tracked holds. Room reset clears this
/// count without releasing holds. Other display clients own their own counts.
static u16 _gPadInputMenuHoldCount;

/// Freezes automatic menu-lock decisions while nonzero (0 automatic, 1 held).
///
/// SET_AND_HOLD requests set this latch; explicit SET/CLEAR and reset release
/// it. Logical suppression publication continues while the decision is held.
static u8 _gPadInputAutomaticMenuLockHeld;

/* Define BSS before API headers to preserve first-declaration order. */

s16 gPadInputMenuUnlockUpdates;

/// Player-present input updates remaining with logical Triangle forcibly removed.
///
/// Reset and automatic menu ineligibility arm four updates. Decremented after
/// normal publication, including the arming update; zero leaves Triangle alone.
s16 _gPadInputWheelBlockUpdates;

#include "gameplay/pad_input.h"

enum {
    PAD_INPUT_LAYOUT_A                    = 0,
    PAD_INPUT_LAYOUT_B                    = 1,
    PAD_INPUT_LAYOUT_C                    = 2,
    PAD_INPUT_MOVEMENT_WALK               = 0,
    PAD_INPUT_MOVEMENT_RUN                = 1,
    PAD_INPUT_PLAYER_STATE_AIM            = 2,
    PAD_INPUT_PLAYER_STATE_ATTACHMENT_USE = 6,
    PAD_INPUT_LOGICAL_BUTTON_MASK         = 0xFFFF,
    PAD_INPUT_MENU_UNLOCK_UPDATES         = 8,
    PAD_INPUT_WHEEL_BLOCK_UPDATES         = 4,
    PAD_INPUT_INTERACTION_REARM_DELAY     = 10,
    PAD_INPUT_STICK_RUN_THRESHOLD         = PAD_STICK_FULL_SCALE * 29 / 32,
};

static u16 _padInputRemapButtons(const GameActor* player, u16 buttons);

/// Layout A physical-bit to logical-bit indices.
///
/// Input indices and stored output indices are 0..15, covering a complete
/// active-high pad mask. Multiple inputs may map to one logical button.
static const u8 _gPadInputLayoutAMap[16] __attribute__((section(".data"))) = {
    0,
    1,
    2,
    3,
    4,
    6,
    5,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
};

/// Layout B physical-bit to logical-bit indices.
///
/// Input indices and stored output indices are 0..15, covering a complete
/// active-high pad mask. Multiple inputs may map to one logical button.
static const u8 _gPadInputLayoutBMap[16] __attribute__((section(".data"))) = {
    0,
    1,
    2,
    3,
    4,
    7,
    5,
    6,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
};

/// Layout C physical-bit to logical-bit indices before aiming or outside normal mode.
///
/// Input indices and stored output indices are 0..15, covering a complete
/// active-high pad mask. Multiple inputs may map to one logical button.
static const u8 _gPadInputLayoutCMap[16] __attribute__((section(".data"))) = {
    0,
    4,
    2,
    7,
    7,
    6,
    5,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
};

/// Layout C physical-bit to logical-bit indices in normal states at or above aiming.
///
/// Input indices and stored output indices are 0..15, covering a complete
/// active-high pad mask. Multiple inputs may map to one logical button.
static const u8 _gPadInputLayoutCAlternateMap[16] __attribute__((section(".data"))) = {
    0,
    4,
    2,
    7,
    1,
    6,
    5,
    3,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
};

/// Publishes remapped input, applying suppression and an additional logical mask.
///
/// All three samples use active-high physical pad bits. The borrowed live
/// player selects the saved layout's mapping; it must remain valid throughout
/// the three remap calls. `keepMask` selects logical output bits (all bits for
/// the normal publish, all except Triangle during wheel suppression). Each
/// output truncates to the session's u16 mask after both filters. Samples are
/// mapped independently, so the published edges are not recomputed from held
/// buttons. No pointer is retained and suppression state is not changed.
static inline void _padInputPublishButtons(const GameActor* player, u16 heldButtons, u16 pressedButtons, u16 releasedButtons, s32 keepMask)
{
    gGameSession->padHeld     = _padInputRemapButtons(player, heldButtons) & ~_gPadInputSuppressionMask & keepMask;
    gGameSession->padPressed  = _padInputRemapButtons(player, pressedButtons) & ~_gPadInputSuppressionMask & keepMask;
    gGameSession->padReleased = _padInputRemapButtons(player, releasedButtons) & ~_gPadInputSuppressionMask & keepMask;
}

#ifdef PC_PORT
/* PC: movement options from pe2.toml's [input] (the host's controls.rs):
 * bit 0 camera-relative ("modern") movement, bit 1 quick turn. */
extern u32 pc_input_options(void);
/* PE2_TRACE=move: one line per tick of modern movement. */
extern void pc_trace_movement(s32 sx, s32 sy, s32 want, s32 yaw, s32 x, s32 z);
/* Input recordings (replay.rs): where the player is, each update. */
extern void pc_replay_player(s32 x, s32 z, s32 yaw);

#define PC_INPUT_MODERN     1
#define PC_INPUT_QUICK_TURN 2

#define PC_PAD_DIRS (PAD_BUTTON_UP | PAD_BUTTON_RIGHT | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT)

/* The lock-on button, as remapped (held in battle, it picks targets). */
#define PC_LOGICAL_LOCK 0x80

#define PC_ABS(x) ((x) < 0 ? -(x) : (x))

/* Yaw per tick (4096 a turn): turning toward the stick, as quick as the
 * Resident Evil remake's alternate controls (half a turn in four ticks),
 * and a quick turn. */
#define PC_MODERN_TURN_STEP 0x200
#define PC_QUICK_TURN_STEP  0x155

/* Modern movement walks once the player faces within this of where the
 * stick points (90 degrees), turning on the way. */
#define PC_MODERN_WALK_ARC 0x400

/* A stick turned this far from where it was when the camera was taken
 * (45 degrees) takes the camera again. */
#define PC_MODERN_RETAKE_ARC 0x200

/* Yaw a quick turn still has to turn. */
static s16 pcQuickTurn;

/* The camera's ground axes modern movement goes by (right, then forward,
 * x and z), taken when a direction is first pushed and kept while the
 * stick holds it, so camera cuts do not change where the player is
 * heading; released or turned elsewhere (PC_MODERN_RETAKE_ARC), the stick
 * goes by the camera of the moment again, as in the Resident Evil remake.
 * pcMoveStickYaw: the stick's own direction when they were taken. */
static s32 pcMoveAxes[4];
static s32 pcMoveHeld;
static s32 pcMoveStickYaw;

/* The ground direction (x, z) of a row of the view rotation, scaled to
 * 4096; 0 when the row is vertical. */
static void pcViewGroundAxis(s32 row, s32* x, s32* z)
{
    s32 vx  = gGfxViewRotCoord.coord.m[row][0];
    s32 vz  = gGfxViewRotCoord.coord.m[row][2];
    s32 len = SquareRoot0(vx * vx + vz * vz);
    if (len == 0) {
        *x = *z = 0;
        return;
    }
    *x = vx * 4096 / len;
    *z = vz * 4096 / len;
}

/// PC: modern movement and quick turn, applied to the button word `held`
/// this frame (d-pad bits included, the stick folded in as the original
/// does) while the player is free to walk: exploring (state 0) or armed in
/// battle (state 2), where releasing the directions lets the game turn back
/// to the lock target. In battle, while the lock button is held, the
/// directions keep their original job (left and right pick another
/// target).
///
/// Modern movement, as the Resident Evil remake's alternate controls: the
/// stick (or the d-pad) points where to go, relative to the camera. The
/// player turns toward it quickly (rotation.vy, whose forward is (sin, cos)
/// in world x, z), walking as soon as facing within 90 degrees; a slight
/// push walks and a full one runs (the d-pad runs with the run button).
///
/// Quick turn (tank movement): back and the run button turn the player
/// half a turn in a few ticks.
static u16 pc_movement(const PadState* pad, GameActor* actor, u16 held)
{
    u32 options = pc_input_options();
    u16 run     = gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout == PAD_INPUT_LAYOUT_B ? PAD_BUTTON_SQUARE
                                                                                            : PAD_BUTTON_CIRCLE;
    s32 sx = 0, sy = 0, analog = 0;
    s32 rx, rz, fx, fz, dx, dz, yaw, diff, step, magnitude, stickYaw;
    u16 out;

    if (actor->mode != GAME_ACTOR_MODE_NORMAL || (actor->state != 0 && actor->state != PAD_INPUT_PLAYER_STATE_AIM) ||
        _gPadInputMenuLocked != 0 || actor->movementInputDisabled != 0) {
        /* Doors, events, menus: the next push takes the camera then. */
        pcQuickTurn = 0;
        pcMoveHeld  = 0;
        return held;
    }
    if (pcQuickTurn != 0) {
        step                = pcQuickTurn < PC_QUICK_TURN_STEP ? pcQuickTurn : PC_QUICK_TURN_STEP;
        actor->rotation.vy  = (actor->rotation.vy + step) & 0xFFF;
        pcQuickTurn        -= step;
        return held & ~(PC_PAD_DIRS | run);
    }
    /* Battle: the lock button held picks targets. */
    if (actor->state == PAD_INPUT_PLAYER_STATE_AIM && (_padInputRemapButtons(actor, pad->buttons) & PC_LOGICAL_LOCK)) {
        pcMoveHeld = 0;
        return held;
    }
    if (!(options & PC_INPUT_MODERN)) {
        if ((options & PC_INPUT_QUICK_TURN) && (held & PAD_BUTTON_DOWN) && (pad->pressedButtons & run)) {
            pcQuickTurn = 0x800;
            return held & ~(PC_PAD_DIRS | run);
        }
        return held;
    }

    if (pad->inputFormat == PAD_INPUT_FORMAT_ANALOG) {
        sx = pad->stickAxes[PAD_STICK_LEFT_X];
        sy = pad->stickAxes[PAD_STICK_LEFT_Y];
    }
    if (PC_ABS(sx) < PAD_STICK_FULL_SCALE / 4 && PC_ABS(sy) < PAD_STICK_FULL_SCALE / 4) {
        sx = (pad->buttons & PAD_BUTTON_RIGHT) ? PAD_STICK_FULL_SCALE : (pad->buttons & PAD_BUTTON_LEFT) ? -PAD_STICK_FULL_SCALE : 0;
        sy = (pad->buttons & PAD_BUTTON_DOWN) ? PAD_STICK_FULL_SCALE : (pad->buttons & PAD_BUTTON_UP) ? -PAD_STICK_FULL_SCALE : 0;
    } else {
        analog = 1;
    }
    /* The pad's own buttons, without the directions the stick added. */
    out = pad->buttons & ~PC_PAD_DIRS;
    if (sx == 0 && sy == 0) {
        pcMoveHeld = 0;
        return out;
    }

    /* Stick right is the camera's right, stick up its forward, on the
     * ground: rows 0 and 2 of the world-to-camera rotation. */
    stickYaw = ratan2(sx, -sy) & 0xFFF;
    if (pcMoveHeld && PC_ABS(((stickYaw - pcMoveStickYaw + 0x800) & 0xFFF) - 0x800) > PC_MODERN_RETAKE_ARC) {
        pcMoveHeld = 0;
    }
    if (!pcMoveHeld) {
        pcMoveHeld     = 1;
        pcMoveStickYaw = stickYaw;
        pcViewGroundAxis(0, &pcMoveAxes[0], &pcMoveAxes[1]);
        pcViewGroundAxis(2, &pcMoveAxes[2], &pcMoveAxes[3]);
    }
    rx  = pcMoveAxes[0];
    rz  = pcMoveAxes[1];
    fx  = pcMoveAxes[2];
    fz  = pcMoveAxes[3];
    dx  = sx * rx - sy * fx;
    dz  = sx * rz - sy * fz;
    yaw = ratan2(dx, dz) & 0xFFF;

    diff = ((yaw - actor->rotation.vy + 0x800) & 0xFFF) - 0x800;
    step = diff;
    if (step > PC_MODERN_TURN_STEP) {
        step = PC_MODERN_TURN_STEP;
    } else if (step < -PC_MODERN_TURN_STEP) {
        step = -PC_MODERN_TURN_STEP;
    }
    pc_trace_movement(sx, sy, yaw, actor->rotation.vy, actor->previousPosition.vx, actor->previousPosition.vz);
    actor->rotation.vy = (actor->rotation.vy + step) & 0xFFF;
    if (PC_ABS(diff - step) < PC_MODERN_WALK_ARC) {
        out |= PAD_BUTTON_UP;
    }

    /* A full push runs (or walks, with running as the default), as the
     * original's analog handling does, outside battle. */
    if (analog && actor->state == 0) {
        magnitude = SquareRoot0(sx * sx + sy * sy);
        if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.moveMode == PAD_INPUT_MOVEMENT_WALK) {
            if (magnitude > PAD_INPUT_STICK_RUN_THRESHOLD) {
                out |= run;
            }
        } else if (magnitude > PAD_INPUT_STICK_RUN_THRESHOLD) {
            out &= ~run;
        } else {
            out |= run;
        }
    }
    return out;
}
#endif

void padInputUpdate(void)
{
    const PadState*     pad;
    const PlayerStatus* playerStatus;
    const Task*         playerTask;
    const GameActor*    player;
    u16                 heldButtons;
    u16                 pressedButtons;
    u16                 releasedButtons;

    pad          = &gPadStates[0];
    playerStatus = &gPlayerStatus;
    playerTask   = gPlayerActorTasks[PLAYER_ACTOR_TASK_PLAYER];
    if (playerTask == NULL) {
        return;
    }
    player = playerTask->work;
    padScriptClearHalt();
    // Explicit held suppression freezes the automatic menu-lock decision.
    if (_gPadInputAutomaticMenuLockHeld == false) {
        if (player->mode == GAME_ACTOR_MODE_NORMAL && gGameSession->eventState == 0 && gGameSession->cutsceneHold == 0 &&
            player->state != PAD_INPUT_PLAYER_STATE_ATTACHMENT_USE && playerStatus->hp > 0 && gGameSession->deathVariant == 0) {
            if (gPadInputMenuUnlockUpdates > 0) {
                gPadInputMenuUnlockUpdates--;
                _gPadInputMenuLocked = true;
            } else {
                _gPadInputMenuLocked = false;
            }
        } else {
            _gPadInputMenuLocked              = true;
            _gPadInputWheelBlockUpdates       = PAD_INPUT_WHEEL_BLOCK_UPDATES;
            gPadInputMenuUnlockUpdates        = PAD_INPUT_MENU_UNLOCK_UPDATES;
            gDirectionInteractionRearmUpdates = PAD_INPUT_INTERACTION_REARM_DELAY;
        }
        if (_gPadInputMenuLocked == true && _gPadInputPreviousMenuLocked == false) {
            _gPadInputAutomaticMenuLockHeld = false;
            _gPadInputSuppressionMask      |= PAD_INPUT_SUPPRESS_MENU;
        } else if (_gPadInputMenuLocked == false && _gPadInputPreviousMenuLocked == true) {
            _gPadInputAutomaticMenuLockHeld = false;
            _gPadInputSuppressionMask      &= (PAD_INPUT_LOGICAL_BUTTON_MASK ^ PAD_INPUT_SUPPRESS_MENU);
        }
        _gPadInputPreviousMenuLocked = _gPadInputMenuLocked;
    }
    // Only suppression edges acquire or release this module's display holds.
    _gPadInputSuppressionSet          = ~_gPadInputPreviousSuppressionMask & _gPadInputSuppressionMask;
    _gPadInputSuppressionCleared      = _gPadInputPreviousSuppressionMask & ~_gPadInputSuppressionMask;
    _gPadInputPreviousSuppressionMask = _gPadInputSuppressionMask;
    if (gDisplayState.demoScene == DISPLAY_DEMO_NONE) {
        if (_gPadInputSuppressionSet & PAD_INPUT_SUPPRESS_MENU) {
            displayAcquireMenuHold();
            _gPadInputMenuHoldCount++;
        }
        if (_gPadInputSuppressionCleared & PAD_INPUT_SUPPRESS_MENU) {
            while (_gPadInputMenuHoldCount != 0) {
                displayReleaseMenuHold();
                _gPadInputMenuHoldCount--;
            }
        }
    }
    // Analog forward strength selects the saved walk/run modifier before mapping.
    if (pad->inputFormat == PAD_INPUT_FORMAT_ANALOG) {
        heldButtons     = pad->buttons;
        pressedButtons  = pad->pressedButtons;
        releasedButtons = pad->releasedButtons;
        if (pad->stickAxes[PAD_STICK_LEFT_Y] < -PAD_STICK_DIRECTION_THRESHOLD) {
            heldButtons |= PAD_BUTTON_UP;
            if (player->mode != GAME_ACTOR_MODE_NORMAL || player->state < PAD_INPUT_PLAYER_STATE_AIM) {
                if (pad->stickAxes[PAD_STICK_LEFT_Y] < -PAD_INPUT_STICK_RUN_THRESHOLD) {
                    if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.moveMode == PAD_INPUT_MOVEMENT_WALK) {
                        if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout != PAD_INPUT_LAYOUT_B) {
                            heldButtons |= PAD_BUTTON_CIRCLE;
                        } else {
                            heldButtons |= PAD_BUTTON_SQUARE;
                        }
                    } else {
                        // Layouts A and C share Circle; separate arms preserve allocation.
                        if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout == PAD_INPUT_LAYOUT_B) {
                            heldButtons &= (0xFFFF ^ PAD_BUTTON_SQUARE);
                        } else if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout == PAD_INPUT_LAYOUT_A) {
                            heldButtons &= (0xFFFF ^ PAD_BUTTON_CIRCLE);
                        } else {
                            heldButtons &= (0xFFFF ^ PAD_BUTTON_CIRCLE);
                        }
                    }
                } else if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.moveMode == PAD_INPUT_MOVEMENT_RUN) {
                    if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout != PAD_INPUT_LAYOUT_B) {
                        heldButtons |= PAD_BUTTON_CIRCLE;
                    } else {
                        heldButtons |= PAD_BUTTON_SQUARE;
                    }
                }
            }
            if (pad->stickAxes[PAD_STICK_LEFT_X] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
                heldButtons |= PAD_BUTTON_RIGHT;
            } else if (pad->stickAxes[PAD_STICK_LEFT_X] < -PAD_STICK_DIRECTION_THRESHOLD) {
                heldButtons |= PAD_BUTTON_LEFT;
            }
        } else if (pad->stickAxes[PAD_STICK_LEFT_Y] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
            heldButtons |= PAD_BUTTON_DOWN;
            if (pad->stickAxes[PAD_STICK_LEFT_X] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
                heldButtons |= PAD_BUTTON_RIGHT;
            } else if (pad->stickAxes[PAD_STICK_LEFT_X] < -PAD_STICK_DIRECTION_THRESHOLD) {
                heldButtons |= PAD_BUTTON_LEFT;
            }
        } else {
            if (pad->stickAxes[PAD_STICK_LEFT_X] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
                heldButtons |= PAD_BUTTON_RIGHT;
            } else if (pad->stickAxes[PAD_STICK_LEFT_X] < -PAD_STICK_DIRECTION_THRESHOLD) {
                heldButtons |= PAD_BUTTON_LEFT;
            }
        }
    } else {
        heldButtons     = pad->buttons;
        pressedButtons  = pad->pressedButtons;
        releasedButtons = pad->releasedButtons;
    }
#ifdef PC_PORT
    pc_replay_player(player->previousPosition.vx, player->previousPosition.vz, player->rotation.vy);
    heldButtons = pc_movement(pad, (GameActor*)player, heldButtons);
#endif
    // Suppression masks address logical buttons, so apply them after remapping.
    _padInputPublishButtons(player, heldButtons, pressedButtons, releasedButtons, ~0);
    if (_gPadInputWheelBlockUpdates != 0) {
        _gPadInputWheelBlockUpdates--;
        _padInputPublishButtons(player, heldButtons, pressedButtons, releasedButtons, ~PAD_BUTTON_TRIANGLE);
    }
}

/// Converts an active-high pad mask to logical buttons for the saved layout.
///
/// Borrows the live player to select layout C's alternate mapping in normal
/// states at or above aiming (2). Each table maps all sixteen input bits to
/// output bit indices; multiple inputs may set the same output. Invalid layouts
/// return zero.
static u16 _padInputRemapButtons(const GameActor* player, u16 buttons)
{
    /// Adds remapped bits from a complete sixteen-entry table of bit indices 0..15.
    ///
    /// Scalar arguments must be side-effect-free; destination and cursor are
    /// distinct lvalues. Arguments are reused per bit; the destination is not cleared.
    /// `buttonMap` must be an actual array, not a pointer: its element count
    /// determines the input-bit domain. All current maps cover bits 0..15.
#define PAD_INPUT_REMAP_BUTTON_BITS(sourceButtons, buttonMap, destinationButtons, bitCursor) \
    do {                                                                                     \
        for ((bitCursor) = 0; (bitCursor) < ARRAY_SIZE(buttonMap); (bitCursor)++) {          \
            if (((sourceButtons) >> (bitCursor)) & 1) {                                      \
                (destinationButtons) |= 1 << (buttonMap)[(bitCursor)];                       \
            }                                                                                \
        }                                                                                    \
    } while (0)

    u16 mappedButtons;
    s32 buttonBit;

    mappedButtons = 0;
    switch (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout) {
        case PAD_INPUT_LAYOUT_A:
            PAD_INPUT_REMAP_BUTTON_BITS(buttons, _gPadInputLayoutAMap, mappedButtons, buttonBit);
            break;
        case PAD_INPUT_LAYOUT_B:
            PAD_INPUT_REMAP_BUTTON_BITS(buttons, _gPadInputLayoutBMap, mappedButtons, buttonBit);
            break;
        case PAD_INPUT_LAYOUT_C:
            if (player->mode == GAME_ACTOR_MODE_NORMAL && player->state >= PAD_INPUT_PLAYER_STATE_AIM) {
                PAD_INPUT_REMAP_BUTTON_BITS(buttons, _gPadInputLayoutCAlternateMap, mappedButtons, buttonBit);
            } else {
                PAD_INPUT_REMAP_BUTTON_BITS(buttons, _gPadInputLayoutCMap, mappedButtons, buttonBit);
            }
            break;
    }
#undef PAD_INPUT_REMAP_BUTTON_BITS
    return mappedButtons;
}

void padInputChangeSuppression(u8 command, s32 buttonMask)
{
    switch (command) {
        case PAD_INPUT_SUPPRESSION_SET:
        case PAD_INPUT_SUPPRESSION_SET_ALIAS:
            _gPadInputAutomaticMenuLockHeld = false;
            _gPadInputSuppressionMask      |= buttonMask;
            break;
        case PAD_INPUT_SUPPRESSION_SET_AND_HOLD:
            _gPadInputSuppressionMask      |= buttonMask;
            _gPadInputAutomaticMenuLockHeld = true;
            break;
        case PAD_INPUT_SUPPRESSION_CLEAR:
        case PAD_INPUT_SUPPRESSION_CLEAR_ALIAS:
            _gPadInputAutomaticMenuLockHeld = false;
            _gPadInputSuppressionMask      &= ~buttonMask;
        case PAD_INPUT_SUPPRESSION_KEEP:
            break;
    }
}

void padInputResetSuppression(void)
{
    _gPadInputMenuHoldCount           = 0;
    _gPadInputSuppressionMask         = 0;
    _gPadInputPreviousSuppressionMask = 0;
    _gPadInputSuppressionSet          = 0;
    _gPadInputSuppressionCleared      = 0;
    _gPadInputMenuLocked              = false;
    _gPadInputPreviousMenuLocked      = false;
    _gPadInputAutomaticMenuLockHeld   = false;
    gPadInputMenuUnlockUpdates        = PAD_INPUT_MENU_UNLOCK_UPDATES;
    _gPadInputWheelBlockUpdates       = PAD_INPUT_WHEEL_BLOCK_UPDATES;
}
