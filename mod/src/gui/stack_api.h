/* Item stack sizes, the interface any mod that raises them exports.
 *
 * The same header ships with Private Storage Master and with Stack Master, and
 * both export exactly these names. A caller in the game process (Master
 * Looter's Stacks tab) finds a provider by looking for each module it knows
 * with GetModuleHandleW, then GetProcAddress on the functions below:
 *
 *     StackMaster.asi              the standalone mod
 *     PrivateStorageMaster.asi     the same feature inside the storage mod
 *
 * More than one may answer. Ask each for its status and edit the one whose
 * `applying` is 1: that is the mod actually changing the limits. Stack Master
 * takes precedence by design, so when it is installed Private Storage Master
 * reports applying 0 and leaves stacks alone. If nothing reports applying 1 the
 * feature is off everywhere, and StackApplyMultiplier on any provider that
 * answers turns it on from the next launch.
 *
 * Check StackApiVersion() first and use nothing if it differs from
 * STACK_API_VERSION. Every struct starts with its own size; set it before
 * passing one in. Fields are only ever appended, never reordered or removed, so
 * a caller built against an older header stays a prefix of a newer provider's
 * struct: the provider fills what fits, sets `size` to how many bytes it wrote,
 * and a caller checks that against the offset of any field it wants to read. A
 * size smaller than the first published layout is refused with 0.
 *
 * Plain C, no allocation across the boundary, strings copied into the caller's
 * buffers. Everything is safe to call from any thread, every frame.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifndef STACK_API
#define STACK_API   /* a provider defines it as dllexport while building */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define STACK_API_VERSION 1

/* The largest multiple a provider accepts, and the most any one stack holds. */
#define STACK_MAX_MULTIPLIER 1000
#define STACK_CEILING        999999

typedef struct StackStatus
{
    uint32_t size;               /* sizeof(StackStatus) */
    char     provider[24];       /* "Stack Master", "Private Storage Master" */
    char     providerModule[32]; /* "StackMaster.asi" */
    char     version[16];        /* the provider's own version */
    char     gameVersion[16];    /* game versions it was built for */
    int32_t  applying;           /* 1 = this mod is the one changing limits */
    int32_t  standDownReason;    /* STACK_STANDDOWN_*, 0 when applying */
    int32_t  hooked;             /* the item table reader is hooked */
    int32_t  multiplier;         /* in force this launch; 1 means untouched */
    int32_t  multiplierSetting;  /* in the ini, which may differ until a restart */
    int32_t  restartNeeded;      /* the setting differs from this launch */
    int32_t  itemsRaised;        /* item records whose limit was raised */
    int32_t  itemsUnstackable;   /* records left alone: the game does not stack them */
    /* This provider's own limits. Read them from here rather than from the
     * defines above, so a caller built against an older header still shows the
     * numbers the provider is really using. */
    int32_t  ceiling;            /* the most any one stack holds */
    int32_t  maxMultiplier;      /* the largest multiplier it accepts */
    int64_t  biggest;            /* the largest limit written */
    /* ---- added after the first release; everything above is the v1 layout ---- */
    /* 1 when a bigger multiplier can take effect without a restart, which needs
     * this mod to be applying already. A smaller one always waits for the next
     * launch, because a slot holding more than the game allows would be left over
     * the limit. Read it to word the tab; you do not have to act on it, since
     * StackApplyMultiplier does the right thing either way. */
    int32_t  liveRaise;
    /* Added with Stack Master 1.0.6. Not 0 when one size for every stacking
     * item is what sets the limits this launch, rather than the multiplier:
     * every item that stacks holds at least this many, one the game stacks
     * higher keeps its own limit, and `multiplier` is not used. Stack Master's
     * StackSize setting is the only source; Private Storage Master has none and
     * fills less than this. Check `size` before reading it. */
    int32_t  stackSize;
} StackStatus;

/* The first published layout, up to and including `biggest`. A provider accepts
 * any size from here upward. It names the first field added after v1, so leave it
 * alone when appending more. */
#define STACK_STATUS_V1 ((int)offsetof(StackStatus, liveRaise))

#define STACK_STANDDOWN_NONE       0
#define STACK_STANDDOWN_OFF        1   /* the multiplier is 1 */
#define STACK_STANDDOWN_OTHER_MOD  2   /* another provider takes precedence */
#define STACK_STANDDOWN_NO_ANCHOR  3   /* the item table reader was not found */
#define STACK_STANDDOWN_HOOK_FAILED 4
#define STACK_STANDDOWN_TOO_LATE   5   /* the game read its item table first */

/* STACK_API_VERSION of this provider. Call it before anything else. */
STACK_API int StackApiVersion(void);

/* Fills out. Returns 1, or 0 when out is null or its size is unknown. */
STACK_API int StackGetStatus(StackStatus* out);

/* A sentence for a menu saying why stacks are not being changed, for a
 * standDownReason from StackGetStatus. It carries what a number cannot, such as
 * which mod took precedence. Writes something printable whenever out is valid,
 * and returns 1 for a code this provider knows, 0 for one it does not. */
STACK_API int StackStandDownText(int reason, char* out, int outLen);
/* For reason 0 the sentence also says when stackSize, not the multiplier, is
 * what sets the limits, so a menu that predates that field still shows it. */

/* The multiplier in the ini, 1 when off. 0 means the call failed. While a
 * provider's stackSize is set the multiplier is saved but not used. */
STACK_API int StackGetMultiplier(void);

/* Writes the multiplier to the provider's own ini, between 1 and the
 * maxMultiplier it reports, so a caller never touches a file itself.
 *
 * A bigger multiplier than the one in force is applied to the limits already in
 * memory straight away, when the provider can (liveRaise) and the player is in
 * free play with no storage screen open. Anything else, including every smaller
 * multiplier, takes effect the next time the game starts.
 *
 * Either way the setting is saved and this returns 1. Read StackGetStatus after
 * it: `multiplier` is what is in force now and `restartNeeded` is 0 when the
 * change is already live, 1 when it is waiting for a restart. That is the one
 * thing to tell the player. Returns 0 when the value itself is refused, or
 * when the provider has stackSize set, since then no multiplier would change
 * anything; `why` (which may be null) says which. */
STACK_API int StackApplyMultiplier(int multiplier, char* why, int whyLen);

#ifdef __cplusplus
}
#endif
