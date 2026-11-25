/* Portions derived from XTFrame for CRYENGINE (XUI2D), Copyright (c) 2026 XTFrame contributors, MIT license. */
/* XUI2DL: the frozen C interface between the renderer DLL and its client (CEVPlayer docs/design/M11-xui2dl.md section 4).
   The renderer exports extern "C" const XUI2DLApi* XUI2DL_GetApi(uint32_t version); nullptr when the version differs.
   Two copies exist (the engine patch and the CEVPlayer plug-in); they must stay byte-identical. */
#pragma once

#include <stdint.h>

#define XUI2DL_API_VERSION 1
enum { XUI2DL_CAP_CORE = 1, XUI2DL_CAP_YUV = 2, XUI2DL_CAP_LOADTIME = 4 };
enum { XUI2DL_KIND_SOLID = 0, XUI2DL_KIND_RGBA_PREMULT = 1, XUI2DL_KIND_RGBA_STRAIGHT = 2,
       XUI2DL_KIND_YUV420 = 3, XUI2DL_KIND_YUVA420 = 4 };
enum { XUI2DL_BLEND_REPLACE = 0, XUI2DL_BLEND_SRCOVER = 1 };   /* sources are premultiplied */
enum { XUI2DL_MAX_LAYERS = 16 };

typedef struct XUI2DLRect { float x, y, w, h; } XUI2DLRect;
typedef struct XUI2DLPlane { uint32_t x, y, w, h; } XUI2DLPlane;   /* texels in the packed texture */

typedef struct XUI2DLLayer
{
    uint32_t    size;          /* sizeof(XUI2DLLayer) */
    uint32_t    kind;          /* XUI2DL_KIND_* */
    uint32_t    blend;         /* XUI2DL_BLEND_* */
    int32_t     textureId;     /* engine texture id; ignored for SOLID */
    XUI2DLRect  dst;           /* viewport pixels, origin top-left, y down */
    XUI2DLRect  src;           /* image pixels, row 0 = top row; w <= 0 means the whole image */
    XUI2DLRect  scissor;       /* viewport pixels; w <= 0 means none */
    float       color[4];      /* premultiplied r,g,b,a multiplier; SOLID: the colour itself */
    uint32_t    imageWidth;    /* YUV kinds: W, H of the picture (chapter 61) */
    uint32_t    imageHeight;
    XUI2DLPlane planeY, planeU, planeV, planeA;
    uint32_t    colorMatrix;   /* 0 = BT.601, 1 = BT.709 */
    uint32_t    colorRange;    /* 0 = limited, 1 = full */
    uint32_t    chromaSiting;  /* 0 = left, 1 = center, 2 = top-left */
    uint32_t    reserved;      /* 0 */
} XUI2DLLayer;

typedef struct XUI2DLFrame
{
    uint32_t           size;        /* sizeof(XUI2DLFrame) */
    uint32_t           layerCount;  /* 0..XUI2DL_MAX_LAYERS; 0 clears the retained list */
    const XUI2DLLayer* pLayers;     /* drawn in order, first = bottom */
    XUI2DLRect         viewport;    /* output pixels; w <= 0 means the whole output */
} XUI2DLFrame;

typedef struct XUI2DLStats
{
    uint32_t size;
    uint32_t resourcesReady;   /* device resources exist */
    uint32_t shaderReady;      /* every technique compiled */
    uint32_t layersHeld;       /* layers in the retained list */
    uint32_t outputWidth, outputHeight;   /* of the last executed frame */
    uint64_t framesDrawn;      /* render-thread executions that drew at least one layer */
    uint64_t drawsExecuted;
    uint64_t drawsSkipped;     /* pipeline state not ready, texture gone, unsupported kind */
} XUI2DLStats;

typedef struct XUI2DLApi
{
    uint32_t size;      /* sizeof(XUI2DLApi) */
    uint32_t version;   /* XUI2DL_API_VERSION */
    uint32_t caps;      /* XUI2DL_CAP_* */
    uint32_t reserved;
    int  (*SetFrame)(const XUI2DLFrame* pFrame);   /* main thread; 1 = accepted, 0 = refused (bad size / count / kind) */
    void (*GetStats)(XUI2DLStats* pStats);         /* any thread */
} XUI2DLApi;
