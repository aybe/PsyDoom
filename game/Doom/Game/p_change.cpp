#include "p_change.h"

#include "Doom/Base/i_main.h"
#include "Doom/Base/m_random.h"
#include "Doom/Base/s_sound.h"
#include "Doom/Base/sounds.h"
#include "Doom/Renderer/r_local.h"
#include "Doom/Renderer/r_main.h"
#include "Doom/UI/st_main.h"
#include "g_game.h"
#include "info.h"
#include "p_inter.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_mobj.h"
#include "p_move.h"
#include "PsyDoom/Config/Config.h"
#include "PsyDoom/Game.h"

bool gbNofit;           // If 'true' then one or more things in the test sector undergoing height changes do not fit
bool gbCrushChange;     // If 'true' then the current sector undergoing height changes should crush/damage things when they do not fit

#if PSYDOOM_MODS
    bool gbFloorIsInstantMoving;        // PsyDoom: a flag set if 'T_MovePlane' is moving a floor which instantly reaches its destination (instant floor)
    bool gbCeilingIsInstantMoving;      // PsyDoom: a flag set if 'T_MovePlane' is moving a ceiling which instantly reaches its destination (instant ceiling)
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Clamps the map object's z position to be in a valid range for the sector it is within using the current floor/ceiling height.
// Returns 'true' if the object can fit vertically in it's current sector, 'false' otherwise (should be crushed).
//------------------------------------------------------------------------------------------------------------------------------------------
bool P_ThingHeightClip(mobj_t& mobj) noexcept {
    // Is the thing currently on the floor?
    const fixed_t oldFloorZ = mobj.floorz;
    const bool bWasOnFloor = (oldFloorZ == mobj.z);

    // PsyDoom: for the thing height clip don't collide with other map objects if the 'sprite vertical warp' bugfix is enabled.
    // These inter-thing collisions can prevent the sprite from being at the correct height during this adjustment.
    #if PSYDOOM_MODS
        const bool bOld_NoMobjCollide = (mobj.flags & MF_NO_MOBJ_COLLIDE);  // Restore this flag's value later!

        if (Game::gSettings.bFixSpriteVerticalWarp) {
            mobj.flags |= MF_NO_MOBJ_COLLIDE;
        }
    #endif

    // Get the current floor/ceiling Z values for the thing and update
    P_CheckPosition(mobj, mobj.x, mobj.y);
    mobj.floorz = gTmFloorZ;
    mobj.ceilingz = gTmCeilingZ;

    // PsyDoom: restore this flag if we modified it for the 'sprite vertical warp' bugfix
    #if PSYDOOM_MODS
        if (Game::gSettings.bFixSpriteVerticalWarp) {
            if (!bOld_NoMobjCollide) {
                mobj.flags &= ~MF_NO_MOBJ_COLLIDE;
            }
        }
    #endif

    // Things that were on the floor previously rise and fall as the sector floor rises and falls.
    // Otherwise, if a floating thing, clip against the ceiling.
    const fixed_t oldZ = mobj.z;

    #if PSYDOOM_MODS
        // PsyDoom: snap the map object's z position if it was pushed due to an instantly moving floor or ceiling
        bool bSnapDueToInstantMovingSector = false;
    #endif

    if (bWasOnFloor) {
        mobj.z = mobj.floorz;

        #if PSYDOOM_MODS
            bSnapDueToInstantMovingSector = gbFloorIsInstantMoving;
        #endif
    }
    else if (oldZ + mobj.height > mobj.ceilingz) {
        mobj.z = mobj.ceilingz - mobj.height;

        #if PSYDOOM_MODS
            bSnapDueToInstantMovingSector = gbCeilingIsInstantMoving;
        #endif
    }

    // Record that the player's view was shifted by this change if it's the current player.
    // Need to adjust interpolation accordingly if that is the case, since the world ticks at a slower rate (15 Hz) than the player (30 Hz).
    // Movements due to world motion must be interpolated over a longer period of time...
    #if PSYDOOM_MODS
        if (mobj.player && (mobj.player == &gPlayers[gCurPlayerIndex])) {
            gViewPushedZ += mobj.z - oldZ;
        }
    #endif

    // PsyDoom: if the thing was moved by the sector and the interpolation of sectors is not enabled then snap the z motion of the object.
    // Also snap if it's being pushed by an instantly moving floor or ceiling:
    #if PSYDOOM_MODS
        if ((!Config::gbInterpolateSectors) || bSnapDueToInstantMovingSector) {
            if (oldZ != mobj.z) {
                mobj.z.snap();
            }
        }
    #endif

    return (mobj.height <= mobj.ceilingz - mobj.floorz);    // Is there enough vertical room for the thing?
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Does updates for one map object after the sector it is contained within has had it's floor or ceiling height changed.
// Clips the map object to the valid height range of the sector, and applies crushing where required.
// Sets 'gbNofit' to 'false' also if the thing does not fit in the height range of the sector.
//------------------------------------------------------------------------------------------------------------------------------------------
bool PIT_ChangeSector(mobj_t& mobj) noexcept {
    // Clip the thing to the height range of the sector: if it fits then we have nothing else to do
    if (P_ThingHeightClip(mobj))
        return true;

    // Turn bodies into gibs
    if (mobj.health <= 0) {
        P_SetMobjState(mobj, S_GIBS);
        S_StartSound(&mobj, sfx_slop);

        mobj.height = 0;    // This prevents the height clip test from failing again and triggering more crushing
        mobj.radius = 0;

        // PsyDoom: fix a bug where gibs become blocking if the monster is crushed during it's death sequence 
        #if PSYDOOM_MODS
            if (Game::gSettings.bFixBlockingGibsBug) {
                mobj.flags &= ~MF_SOLID;
            }
        #endif

        // If it is the player being gibbed then make the status bar head gib
        if (mobj.player == &gPlayers[gCurPlayerIndex]) {
            gStatusBar.gotgibbed = true;
        }

        return true;
    }

    // Crush and destroy dropped items
    if (mobj.flags & MF_DROPPED) {
        P_RemoveMobj(mobj);
        return true;
    }

    // If the thing is not shootable then don't do anything more to it.
    // This will be the case for decorative things etc.
    if ((mobj.flags & MF_SHOOTABLE) == 0)
        return true;

    // Things in the current sector do not fit into it's height range
    gbNofit = true;

    // If the sector crushes and it's every 4th tic then do some damage to the thing
    if (gbCrushChange && ((gGameTic & 3) == 0)) {
        P_DamageMobj(mobj, nullptr, nullptr, 10);

        // Spawn some blood and randomly send it off in different directions
        mobj_t& blood = *P_SpawnMobj(mobj.x, mobj.y, mobj.height / 2 + mobj.z, MT_BLOOD);
        blood.momx = P_SubRandom() * (FRACUNIT / 16);
        blood.momy = P_SubRandom() * (FRACUNIT / 16);
    }

    return true;    // Continue iterating through things in the blockmap
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Called after the height of the given sector changes, either by it's floor or ceiling moving up or down.
// Clips all things contained in the sector to the new height range, and crushes things also (if desired).
// Returns 'true' if one or more things in the sector did not fit into it's height range.
//------------------------------------------------------------------------------------------------------------------------------------------
bool P_ChangeSector(sector_t& sector, const bool bCrunch) noexcept {
    // Force all players to do a noise alert again next time they fire.
    // Because of the height change with this sector, new gaps might have opened...
    for (int32_t i = MAXPLAYERS - 1; i >= 0; --i) {
        gPlayers[i].lastsoundsector = nullptr;
    }

    // Initially everything fits in the sector and save whether to crush for the blockmap iterator
    gbNofit = false;
    gbCrushChange = bCrunch;

    // Clip the heights of all things in the updated sector and crush things where appropriate.
    // Note that this crude test may pull in things in other sectors, which could be included in the results also. Generally that's okay however!
    const int32_t bmapLx = sector.blockbox[BOXLEFT];
    const int32_t bmapRx = sector.blockbox[BOXRIGHT];
    const int32_t bmapTy = sector.blockbox[BOXTOP];
    const int32_t bmapBy = sector.blockbox[BOXBOTTOM];

    for (int32_t x = bmapLx; x <= bmapRx; ++x) {
        for (int32_t y = bmapBy; y <= bmapTy; ++y) {
            P_BlockThingsIterator(x, y, PIT_ChangeSector);
        }
    }

    return gbNofit;     // Did all the things in the sector fit?
}
