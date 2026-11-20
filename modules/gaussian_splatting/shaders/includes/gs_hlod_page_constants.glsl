#ifndef GS_HLOD_PAGE_CONSTANTS_GLSL
#define GS_HLOD_PAGE_CONSTANTS_GLSL

// Shared by the HLOD host configuration and paged shader addressing.
#define GS_HLOD_PAGE_SHIFT 10u
#define GS_HLOD_PAGE_SPLATS (1u << GS_HLOD_PAGE_SHIFT)
#define GS_HLOD_PAGE_MASK (GS_HLOD_PAGE_SPLATS - 1u)

#endif
