//------------------------------------------------------------------------------------------------------------------------------------------
// Various miscellaneous utility functions for the new Vulkan world renderer
//------------------------------------------------------------------------------------------------------------------------------------------
#if PSYDOOM_VULKAN_RENDERER

#include "rv_utils.h"

#include "Doom/Base/i_main.h"
#include "Doom/Base/w_wad.h"
#include "Doom/Game/doomdata.h"
#include "Doom/Game/g_game.h"
#include "Doom/Renderer/r_data.h"
#include "Doom/Renderer/r_local.h"
#include "Doom/Renderer/r_main.h"
#include "Gpu.h"
#include "PsyDoom/Vulkan/VDrawing.h"
#include "PsyDoom/Vulkan/VTypes.h"
#include "PsyQ/LIBGPU.h"
#include "rv_bsp.h"
#include "rv_main.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert a 16.16 fixed point number to float
//------------------------------------------------------------------------------------------------------------------------------------------
float RV_FixedToFloat(const fixed_t num) noexcept {
    return (float)(num * (1.0f / 65536.0f));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert a floating point number to 16.16 fixed
//------------------------------------------------------------------------------------------------------------------------------------------
fixed_t RV_FloatToFixed(const float num) noexcept {
    return (fixed_t)(num * 65536.0f);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert an angle to a float (radians)
//------------------------------------------------------------------------------------------------------------------------------------------
float RV_AngleToFloat(const angle_t angle) noexcept {
    const double normalized = (double) angle / (double(UINT32_MAX) + 1.0);
    const double twoPiRange = normalized * RV_2PI<double>;
    return (float) twoPiRange;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Convert a radians angle to Doom's 32-bit binary angle format
//------------------------------------------------------------------------------------------------------------------------------------------
angle_t RV_FloatToAngle(const float angle) noexcept {
    const double twoPiRange = std::fmod((double) angle, RV_2PI<double>);
    const double normalized = twoPiRange / RV_2PI<double>;
    const double intRange = normalized * (double(UINT32_MAX) + 1.0);
    return (angle_t) intRange;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Unpack a CLUT 'id' into an x, y coordinate for the CLUT
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_ClutIdToClutXy(const uint16_t clutId, uint16_t& clutX, uint16_t& clutY) noexcept {
    clutX = (clutId & 0x3Fu) << 4;      // Max coord: 1023, restricted to being on 16 pixel boundaries on the x-axis
    clutY = (clutId >> 6) & 0x3FFu;     // Max coord: 1023
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Unpack various texturing parameters from the texture page id.
// Extracts the texture format, texture page position, and blending mode.
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_TexPageIdToTexParams(
    const uint16_t texPageId,
    Gpu::TexFmt& texFmt,
    uint16_t& texPageX,
    uint16_t& texPageY,
    Gpu::BlendMode& blendMode
) noexcept {
    texFmt = (Gpu::TexFmt)(texPageId & 0x3u);
    blendMode = (Gpu::BlendMode)((texPageId >> 2) & 0x3u);
    texPageX = ((texPageId >> 4) & 0x7Fu) * 64u;
    texPageY = ((texPageId >> 11) & 0x1Fu) * 256u;

    // Convert the texture page coords from 16-bpp coordinates to format native coords
    switch (texFmt) {
        case Gpu::TexFmt::Bpp16:    // Nothing to do for this format!
            break;

        case Gpu::TexFmt::Bpp8:
            texPageX *= 2;
            break;

        case Gpu::TexFmt::Bpp4:
            texPageX *= 4;
            break;

        default:
            ASSERT_FAIL("Unhandled format!");
            break;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the x/y and width/height of the given texture's texture window in 8bpp coords.
// Assumes the texture format is 8-bits per pixel, which will be the case for all textures used in the 3D world.
//
// Note: I had an assert which checked the 8bpp texture assumption but there are some cases with levels where invalid textures sometimes
// causes it the assertion to fail. Because of this I'm removing the assertion.
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_GetTexWinXyWh(const texture_t& tex, uint16_t& texWinX, uint16_t& texWinY, uint16_t& texWinW, uint16_t& texWinH) noexcept {
    const uint16_t texPageId = tex.texPageId;
    const uint16_t texPageX = ((texPageId >> 4) & 0x7Fu) * 64u;
    const uint16_t texPageY = ((texPageId >> 11) & 0x1Fu) * 256u;

    // Note: multiply 'texPageX' by 2 because it is in 16bpp coords and we are dealing with an 8bpp format here
    texWinX = texPageX * 2 + tex.texPageCoordX;
    texWinY = texPageY + tex.texPageCoordY;
    texWinW = tex.width;
    texWinH = tex.height;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Uploads the given texture to VRAM if it needs to be uploaded, otherwise does nothing.
// Assumes the texture has already been added to the texture cache and has an area of VRAM assigned to itself.
// Used for updating animated floor, wall and sky textures.
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_UploadDirtyTex(texture_t& tex) noexcept {
    // Is the texture already uploaded?
    if (tex.uploadFrameNum != TEX_INVALID_UPLOAD_FRAME_NUM)
        return;

    // Decompress the lump data to the temporary buffer if required
    const std::byte* pLumpData;

    const WadLump& texLump = W_GetLump(tex.lumpNum);
    const bool bIsUncompressedLump = texLump.bIsUncompressed;

    if (bIsUncompressedLump) {
        pLumpData = (const std::byte*) texLump.pCachedData;
    } else {
        const void* pCompressedLumpData = texLump.pCachedData;

        #if PSYDOOM_LIMIT_REMOVING
            gTmpBuffer.ensureSize(getDecodedSize(pCompressedLumpData));
            decode(pCompressedLumpData, gTmpBuffer.bytes());
            pLumpData = gTmpBuffer.bytes();
        #else
            if (getDecodedSize(pCompressedLumpData) > TMP_BUFFER_SIZE) {
                I_Error("RV_UploadDirtyTex: lump %d size > 64 KiB!", tex.lumpNum);
            }

            decode(pCompressedLumpData, gTmpBuffer);
            pLumpData = gTmpBuffer;
        #endif
    }

    // Ensure the texture metrics are up-to-date
    R_UpdateTexMetricsFromData(tex, pLumpData, texLump.uncompressedSize);

    // Load the decompressed texture to the required part of VRAM and mark as loaded.
    const SRECT vramRect = getTextureVramRect(tex);
    LIBGPU_LoadImage(vramRect, (uint16_t*)(pLumpData + sizeof(texlump_header_t)));
    tex.uploadFrameNum = gNumFramesDrawn;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the left x and right x coordinates of the given line in normalized device coords and clips against the front plane.
// Returns 'false' if the line was offscreen, in which case the bounds are UNDEFINED and not set.
//
// Notes:
//  (1) I treat Doom's 'y' coordinate as 'z' for all calculations. In most literature and code on the subject 'z' is actually depth so
//      interpreting 'y' as 'z' keeps things more familiar and allows me to just plug the values into existing equations.
//  (2) For 'xc', 'yc', and 'zc' clipspace coordinates, the point will be inside the clip cube if the
//      following comparisons against clipspace w ('wc') hold:
//          -wc <= xc && xc <= wc
//          -wc <= yc && yc <= wc
//          -wc <= zc && zc <= wc
//  (3) For the offscreen culling test, the caller is allowed to modify the left and right view frustum plane angles using a multiplier.
//      This is so culling can be made more lenient if required, allowing lines that are *almost* onscreen to pass the culling test.
//      This can be used to help avoid sprite pop-in issues for subsectors that are at the edges of the screen where their contained
//      sprites extend across subsectors that ARE in view.
//------------------------------------------------------------------------------------------------------------------------------------------
bool RV_GetLineNdcBounds(
    const float p1x,
    const float p1y,
    const float p2x,
    const float p2y,
    float& lx,
    float& rx,
    const float lrpCullAdjust
) noexcept {
    // Convert the points to clip space by assuming they are the homogenous vectors (x, 0, y, 1) and
    // multiplying by the view projection transform matrix. Save only the xzw components.
    const float mR0C0 = gViewProjMatrix.e[0][0];
    const float mR0C2 = gViewProjMatrix.e[0][2];
    const float mR0C3 = gViewProjMatrix.e[0][3];
    const float mR2C0 = gViewProjMatrix.e[2][0];
    const float mR2C2 = gViewProjMatrix.e[2][2];
    const float mR2C3 = gViewProjMatrix.e[2][3];
    const float mR3C0 = gViewProjMatrix.e[3][0];
    const float mR3C2 = gViewProjMatrix.e[3][2];
    const float mR3C3 = gViewProjMatrix.e[3][3];

    float p1_clip[3] = {
        p1x * mR0C0 + p1y * mR2C0 + mR3C0,  // x
        p1x * mR0C2 + p1y * mR2C2 + mR3C2,  // z (Doom y)
        p1x * mR0C3 + p1y * mR2C3 + mR3C3   // w
    };

    float p2_clip[3] = {
        p2x * mR0C0 + p2y * mR2C0 + mR3C0,  // x
        p2x * mR0C2 + p2y * mR2C2 + mR3C2,  // z (Doom y)
        p2x * mR0C3 + p2y * mR2C3 + mR3C3   // w
    };

    // Throw out the line if it's on the wrong side of the left, right or front viewing planes
    const bool bP1BehindFrontPlane = (p1_clip[1] < -p1_clip[2]);
    const bool bP2BehindFrontPlane = (p2_clip[1] < -p2_clip[2]);

    const bool bCullLine = (
        ((p1_clip[0] < -p1_clip[2] * lrpCullAdjust) && (p2_clip[0] < -p2_clip[2] * lrpCullAdjust)) ||   // Line outside the left view frustrum plane?
        ((p1_clip[0] > +p1_clip[2] * lrpCullAdjust) && (p2_clip[0] > +p2_clip[2] * lrpCullAdjust)) ||   // Line outside the right view frustrum plane?
        (bP1BehindFrontPlane && bP2BehindFrontPlane)                                                    // Line outside the front view frustrum plane?
    );

    if (bCullLine)
        return false;

    // Clip the line against the front plane if required.
    // Use the method described at: https://chaosinmotion.blog/2016/05/22/3d-clipping-in-homogeneous-coordinates/
    if (bP1BehindFrontPlane != bP2BehindFrontPlane) {
        // Get the distance of p1 and p2 to the front plane
        const float p1FpDist = std::abs(p1_clip[1] + p1_clip[2]);
        const float p2FpDist = std::abs(p2_clip[1] + p2_clip[2]);

        // Compute the intersection time of this line against the front plane
        const float t = p1FpDist / (p1FpDist + p2FpDist);

        // Compute the new x and y coords for intersection point via linear interpolation
        const float newX = p1_clip[0] * (1.0f - t) + p2_clip[0] * t;
        const float newY = p1_clip[1] * (1.0f - t) + p2_clip[1] * t;

        // Save the result of the point we want to move.
        // Note that we set 'w' to '-y' to ensure that 'y' ends up as '-1' in NDC:
        if (bP1BehindFrontPlane) {
            p1_clip[0] = newX;
            p1_clip[1] = newY;
            p1_clip[2] = -newY;
        } else {
            p2_clip[0] = newX;
            p2_clip[1] = newY;
            p2_clip[2] = -newY;
        }
    }

    // Compute the normalized device coords for the two line endpoints and save
    float x1 = p1_clip[0] / p1_clip[2];
    float x2 = p2_clip[0] / p2_clip[2];

    if (x1 > x2) {
        std::swap(x1, x2);
    }

    lx = x1;
    rx = x2;
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Clear the draw order field for each subsector drawn this frame.
// This cleanup is required to be done after doing drawing each frame, so that each subsector is marked as initially not being drawn.
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_ClearSubsecDrawIndexes() noexcept {
    for (subsector_t* pSubsec : gRvDrawSubsecs) {
        pSubsec->vkDrawSubsecIdx = -1;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws a black letterbox in the same vertical region where the status bar will show to cover the new areas exposed by widescreen.
// The status bar looks very strange and sticks out otherwise, and moving it down causes other issues like the weapon feeling too low.
//------------------------------------------------------------------------------------------------------------------------------------------
void RV_DrawWidescreenStatusBarLetterbox() noexcept {
    VDrawing::setDrawPipeline(VPipelineType::Colored);
    VDrawing::addFlatColoredQuad(
        -65536.0f,    +200.0f,  0.0f,
        +65536.0f,    +200.0f,  0.0f,
        +65536.0f,  +65536.0f,  0.0f,
        -65536.0f,  +65536.0f,  0.0f,
        0, 0, 0
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Helper: tells if the specified lines intersect
//------------------------------------------------------------------------------------------------------------------------------------------
bool RV_LinesIntersect(
    const float a1x,
    const float a1y,
    const float a2x,
    const float a2y,
    const float b1x,
    const float b1y,
    const float b2x,
    const float b2y
) noexcept {
    // Compute the intersection of two lines using the following method:
    //  http://paulbourke.net/geometry/pointlineplane/
    // See: "Intersection point of two line segments in 2 dimensions"
    const float adx = a2x - a1x;
    const float ady = a2y - a1y;
    const float bdx = b2x - b1x;
    const float bdy = b2y - b1y;

    const float coef1 = a1y - b1y;
    const float coef2 = a1x - b1x;

    const float numerator1 = bdx * coef1 - bdy * coef2;
    const float numerator2 = adx * coef1 - ady * coef2;
    const float denominator = bdy * adx - bdx * ady;

    // If the denominator is '0' then there is no intersection (parallel lines)
    if (denominator == 0)
        return false;

    // Otherwise the finite line segments intersect if 'timeA' and 'timeB' (time along each intersection line) are both between 0 and 1.
    // Note that these time values can be used with linear interpolation between the line points to get the actual intersect point.
    // We don't need that here though so don't bother...
    const float timeA = numerator1 / denominator;
    const float timeB = numerator2 / denominator;
    const bool bLinesIntersect = ((timeA >= 0.0f) && (timeA <= 1.0f) && (timeB >= 0.0f) && (timeB <= 1.0f));
    return bLinesIntersect;
}

#endif  // #if PSYDOOM_VULKAN_RENDERER
