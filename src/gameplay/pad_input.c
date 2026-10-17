#include "pad_input.h"

#include "types.h"

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

/* Define BSS before API headers to preserve first-declaration order. */
u8 Gp_MenuLockNow;

u8 Gp_MenuLockPrev;

u16 Gp_PadSuppressMask;

u16 Gp_PadSuppressPrev;

u16 Gp_PadSuppressRise;

u16 Gp_PadSuppressFall;

u16 Gp_PadSuppressRefs;

u8 Gp_MenuLockHold;

s16 Gp_MenuLockDelay;

s16 Gp_PadSuppressTimer;

#include "gameplay/pad_input.h"

extern u8 Gp_BtnMap0[16];

extern u8 Gp_BtnMap1[16];

extern u8 Gp_BtnMap2[16];

extern u8 Gp_BtnMap2Alt[16];

static u16 Gp_RemapButtons(GameActor* actor, u16 mask);

u8 Gp_BtnMap0[16] = {
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

u8 Gp_BtnMap1[16] = {
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

u8 Gp_BtnMap2[16] = {
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

u8 Gp_BtnMap2Alt[16] = {
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

#ifdef PC_PORT
/* PC: movement options from pe2.toml's [input] (the host's controls.rs):
 * bit 0 camera-relative ("modern") movement, bit 1 quick turn. */
extern u32 pc_input_options(void);
/* PE2_TRACE=move: one line per tick of modern movement. */
extern void pc_trace_movement(s32 sx, s32 sy, s32 want, s32 yaw, s32 x, s32 z);

#define PC_INPUT_MODERN     1
#define PC_INPUT_QUICK_TURN 2

/* Raw button word bits: the d-pad. */
#define PC_PAD_UP    0x1000
#define PC_PAD_RIGHT 0x2000
#define PC_PAD_DOWN  0x4000
#define PC_PAD_LEFT  0x8000
#define PC_PAD_DIRS  0xF000

#define PC_ABS(x) ((x) < 0 ? -(x) : (x))

/* Yaw per tick: turning toward the stick, and a quick turn (4096 a turn). */
#define PC_MODERN_TURN_STEP 0x100
#define PC_QUICK_TURN_STEP  0x155

/* Yaw a quick turn still has to turn. */
static s16 pcQuickTurn;

/* The camera's ground axes modern movement goes by (right, then forward,
 * x and z), taken when a direction is first pushed and kept until it is
 * released, so camera cuts do not change where the player is heading. */
static s32 pcMoveAxes[4];
static s32 pcMoveHeld;

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

/// PC: modern movement and quick turn, applied to the raw button word
/// `mask` the pad gives this frame (d-pad bits included, the stick folded
/// in as the original does) while the player is free to walk: exploring
/// (state 0) or armed in battle (state 2), where releasing the directions
/// lets the game turn back to the lock target. In battle, while the lock
/// button is held, the directions keep their original job (left and right
/// pick another target).
///
/// Modern movement: the stick (or the d-pad) points where to go, relative
/// to the camera. The player turns toward it (rotation.vy, whose forward
/// is (sin, cos) in world x, z: the Z axis Gp_UpdatePlayerMove moves
/// along) and walks forward once facing within 60 degrees; a full push
/// runs, as with the original analog handling.
///
/// Quick turn (tank movement): back and the run button turn the player
/// half a turn in a few ticks.
static u16 pc_movement(PadState* pad, GameActor* actor, u16 mask)
{
    u32 options = pc_input_options();
    u16 run     = gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout == 1 ? 0x80 : 0x20;
    s32 sx = 0, sy = 0, analog = 0;
    s32 rx, rz, fx, fz, dx, dz, yaw, diff, step, magnitude;
    u16 out;

    if (actor->mode != GAME_ACTOR_MODE_NORMAL || (actor->state != 0 && actor->state != 2) || Gp_MenuLockNow != 0 ||
        actor->movementInputDisabled != 0) {
        /* Doors, events, menus: the next push takes the camera then. */
        pcQuickTurn = 0;
        pcMoveHeld  = 0;
        return mask;
    }
    if (pcQuickTurn != 0) {
        step                = pcQuickTurn < PC_QUICK_TURN_STEP ? pcQuickTurn : PC_QUICK_TURN_STEP;
        actor->rotation.vy  = (actor->rotation.vy + step) & 0xFFF;
        pcQuickTurn        -= step;
        return mask & ~(PC_PAD_DIRS | run);
    }
    /* Battle: the lock button (logical 0x80) held picks targets. */
    if (actor->state == 2 && (Gp_RemapButtons(actor, pad->buttons) & 0x80)) {
        pcMoveHeld = 0;
        return mask;
    }
    if (!(options & PC_INPUT_MODERN)) {
        if ((options & PC_INPUT_QUICK_TURN) && (mask & PC_PAD_DOWN) && (pad->pressedButtons & run)) {
            pcQuickTurn = 0x800;
            return mask & ~(PC_PAD_DIRS | run);
        }
        return mask;
    }

    if (pad->inputFormat == PAD_INPUT_FORMAT_ANALOG) {
        sx = pad->stickAxes[PAD_STICK_LEFT_X];
        sy = pad->stickAxes[PAD_STICK_LEFT_Y];
    }
    if (PC_ABS(sx) < PAD_STICK_FULL_SCALE / 4 && PC_ABS(sy) < PAD_STICK_FULL_SCALE / 4) {
        sx = (pad->buttons & PC_PAD_RIGHT) ? PAD_STICK_FULL_SCALE : (pad->buttons & PC_PAD_LEFT) ? -PAD_STICK_FULL_SCALE : 0;
        sy = (pad->buttons & PC_PAD_DOWN) ? PAD_STICK_FULL_SCALE : (pad->buttons & PC_PAD_UP) ? -PAD_STICK_FULL_SCALE : 0;
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
    if (!pcMoveHeld) {
        pcMoveHeld = 1;
        pcViewGroundAxis(0, &pcMoveAxes[0], &pcMoveAxes[1]);
        pcViewGroundAxis(2, &pcMoveAxes[2], &pcMoveAxes[3]);
    }
    rx = pcMoveAxes[0];
    rz = pcMoveAxes[1];
    fx = pcMoveAxes[2];
    fz = pcMoveAxes[3];
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
    if (PC_ABS(diff - step) <= 0x2AA) {
        out |= PC_PAD_UP;
    }

    /* A full push runs (or walks, with running as the default), as the
     * original's analog handling does, outside battle. */
    if (analog && actor->state == 0) {
        magnitude = SquareRoot0(sx * sx + sy * sy);
        if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.moveMode == 0) {
            if (magnitude > 0xE80) {
                out |= run;
            }
        } else if (magnitude > 0xE80) {
            out &= ~run;
        } else {
            out |= run;
        }
    }
    return out;
}
#endif

void Gp_UpdatePadInput(void)
{
    PadState*     pad;
    PlayerStatus* cfg;
    Task*         work;
    GameActor*    actor;
    u16           mask;
    register u16  pressedButtons PSX_REG("s2"); // pinned: global-alloc otherwise ranks `actor` just above %hi(gGameSession) and gives it $s2
    u16           releasedButtons;

    pad  = &gPadStates[0];
    cfg  = &gPlayerStatus;
    work = gPlayerActorTasks[PLAYER_ACTOR_TASK_PLAYER];
    if (work == NULL) {
        return;
    }
    actor = work->work;
    Gp_ClearPadHalt();
    if (Gp_MenuLockHold == 0) {
        if (actor->mode == GAME_ACTOR_MODE_NORMAL && gGameSession->eventState == 0 && gGameSession->cutsceneHold == 0 &&
            actor->state != 6 && cfg->hp > 0 && gGameSession->deathVariant == 0) {
            if (Gp_MenuLockDelay > 0) {
                Gp_MenuLockDelay--;
                Gp_MenuLockNow = 1;
            } else {
                Gp_MenuLockNow = 0;
            }
        } else {
            Gp_MenuLockNow      = 1;
            Gp_PadSuppressTimer = 4;
            Gp_MenuLockDelay    = 8;
            D_80114D08          = 0xA;
        }
        if (Gp_MenuLockNow == 1 && Gp_MenuLockPrev == 0) {
            Gp_MenuLockHold     = 0;
            Gp_PadSuppressMask |= 0x900;
        } else if (Gp_MenuLockNow == 0 && Gp_MenuLockPrev == 1) {
            Gp_MenuLockHold     = 0;
            Gp_PadSuppressMask &= 0xF6FF;
        }
        Gp_MenuLockPrev = Gp_MenuLockNow;
    }
    Gp_PadSuppressRise = ~Gp_PadSuppressPrev & Gp_PadSuppressMask;
    Gp_PadSuppressFall = Gp_PadSuppressPrev & ~Gp_PadSuppressMask;
    Gp_PadSuppressPrev = Gp_PadSuppressMask;
    if (gDisplayState.demoScene == DISPLAY_DEMO_NONE) {
        if (Gp_PadSuppressRise & 0x900) {
            Display_AcquireRef();
            Gp_PadSuppressRefs++;
        }
        if (Gp_PadSuppressFall & 0x900) {
            while (Gp_PadSuppressRefs != 0) {
                Display_ReleaseRef();
                Gp_PadSuppressRefs--;
            }
        }
    }
    if (pad->inputFormat == PAD_INPUT_FORMAT_ANALOG) {
        mask            = pad->buttons;
        pressedButtons  = pad->pressedButtons;
        releasedButtons = pad->releasedButtons;
        if (pad->stickAxes[PAD_STICK_LEFT_Y] < -PAD_STICK_DIRECTION_THRESHOLD) {
            mask |= 0x1000;
            if (actor->mode != GAME_ACTOR_MODE_NORMAL || actor->state < 2) {
                if (pad->stickAxes[PAD_STICK_LEFT_Y] < -0xE80) {
                    if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.moveMode == 0) {
                        if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout != 1) {
                            mask |= 0x20;
                        } else {
                            mask |= 0x80;
                        }
                    } else {
                        if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout == 1) {
                            mask &= 0xFF7F;
                        } else {
                            mask &= 0xFFDF;
                        }
                    }
                } else if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.moveMode == 1) {
                    if (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout != 1) {
                        mask |= 0x20;
                    } else {
                        mask |= 0x80;
                    }
                }
            }
            if (pad->stickAxes[PAD_STICK_LEFT_X] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
                mask |= 0x2000;
            } else if (pad->stickAxes[PAD_STICK_LEFT_X] < -PAD_STICK_DIRECTION_THRESHOLD) {
                mask |= 0x8000;
            }
        } else if (pad->stickAxes[PAD_STICK_LEFT_Y] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
            mask |= 0x4000;
            if (pad->stickAxes[PAD_STICK_LEFT_X] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
                mask |= 0x2000;
            } else if (pad->stickAxes[PAD_STICK_LEFT_X] < -PAD_STICK_DIRECTION_THRESHOLD) {
                mask |= 0x8000;
            }
        } else {
            if (pad->stickAxes[PAD_STICK_LEFT_X] >= PAD_STICK_DIRECTION_THRESHOLD + 1) {
                mask |= 0x2000;
            } else if (pad->stickAxes[PAD_STICK_LEFT_X] < -PAD_STICK_DIRECTION_THRESHOLD) {
                mask |= 0x8000;
            }
        }
    } else {
        mask            = pad->buttons;
        pressedButtons  = pad->pressedButtons;
        releasedButtons = pad->releasedButtons;
    }
#ifdef PC_PORT
    mask = pc_movement(pad, actor, mask);
#endif
    gGameSession->padHeld     = Gp_RemapButtons(actor, mask) & ~Gp_PadSuppressMask;
    gGameSession->padPressed  = Gp_RemapButtons(actor, pressedButtons) & ~Gp_PadSuppressMask;
    gGameSession->padReleased = Gp_RemapButtons(actor, releasedButtons) & ~Gp_PadSuppressMask;
    if (Gp_PadSuppressTimer != 0) {
        Gp_PadSuppressTimer--;
        gGameSession->padHeld     = Gp_RemapButtons(actor, mask) & ~Gp_PadSuppressMask & ~0x10;
        gGameSession->padPressed  = Gp_RemapButtons(actor, pressedButtons) & ~Gp_PadSuppressMask & ~0x10;
        gGameSession->padReleased = Gp_RemapButtons(actor, releasedButtons) & ~Gp_PadSuppressMask & ~0x10;
    }
}

static u16 Gp_RemapButtons(GameActor* actor, u16 mask)
{
    u16 result;
    s32 i;

    result = 0;
    switch (gMcSaveData[MEMORY_CARD_SAVE_LIVE].state.buttonLayout) {
        case 0:
            for (i = 0; i < 0x10; i++) {
                if ((mask >> i) & 1) {
                    result |= 1 << Gp_BtnMap0[i];
                }
            }
            break;
        case 1:
            for (i = 0; i < 0x10; i++) {
                if ((mask >> i) & 1) {
                    result |= 1 << Gp_BtnMap1[i];
                }
            }
            break;
        case 2:
            if (actor->mode == GAME_ACTOR_MODE_NORMAL && actor->state >= 2) {
                for (i = 0; i < 0x10; i++) {
                    if ((mask >> i) & 1) {
                        result |= 1 << Gp_BtnMap2Alt[i];
                    }
                }
            } else {
                for (i = 0; i < 0x10; i++) {
                    if ((mask >> i) & 1) {
                        result |= 1 << Gp_BtnMap2[i];
                    }
                }
            }
            break;
    }
    return result;
}

void func_800E9BDC(u8 arg0, s32 arg1)
{
    switch (arg0) {
        case 1:
        case 5:
            Gp_MenuLockHold     = 0;
            Gp_PadSuppressMask |= arg1;
            break;
        case 3:
            Gp_PadSuppressMask |= arg1;
            Gp_MenuLockHold     = 1;
            break;
        case 0:
        case 2:
            Gp_MenuLockHold     = 0;
            Gp_PadSuppressMask &= ~arg1;
        case 4:
            break;
    }
}

void Gp_ResetMenuLock(void)
{
    Gp_PadSuppressRefs  = 0;
    Gp_PadSuppressMask  = 0;
    Gp_PadSuppressPrev  = 0;
    Gp_PadSuppressRise  = 0;
    Gp_PadSuppressFall  = 0;
    Gp_MenuLockNow      = 0;
    Gp_MenuLockPrev     = 0;
    Gp_MenuLockHold     = 0;
    Gp_MenuLockDelay    = 8;
    Gp_PadSuppressTimer = 4;
}
