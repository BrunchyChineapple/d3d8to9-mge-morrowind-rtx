/**
 * proxydx/d3d8header.h — Shadow header replacing MGE-XE's d3d8header.h
 *
 * When MGE-XE code does #include "proxydx/d3d8header.h", it finds this file
 * (from source/ include path) instead of MGE-XE/src/proxydx/d3d8header.h.
 *
 * This provides the same types and defines that MGE-XE expects, but using
 * d3d8to9's full COM interface classes instead of void* stubs.
 */
#pragma once

#include "d3d9header.h"

// d3d8to9 provides full COM interface classes for these.
// MGE-XE's original header #define'd them to void, which is incompatible.
// We include d3d8to9's definitions via d3d8.hpp instead.
#include "../d3d8.hpp"
#include "../d3d8types.hpp"

// DX8 D3DTEXTURESTAGESTATETYPE enum values
// d3d8to9's d3d8.hpp already defines these, but guard just in case
#ifndef D3DTSS_ADDRESSU
#define D3DTSS_ADDRESSU       13
#define D3DTSS_ADDRESSV       14
#define D3DTSS_BORDERCOLOR    15
#define D3DTSS_MAGFILTER      16
#define D3DTSS_MINFILTER      17
#define D3DTSS_MIPFILTER      18
#define D3DTSS_MIPMAPLODBIAS  19
#define D3DTSS_MAXMIPLEVEL    20
#define D3DTSS_MAXANISOTROPY  21
#define D3DTSS_ADDRESSW       25
#endif

// D3D9 surface desc alias
#ifndef D3DSURFACE_DESC9
#define D3DSURFACE_DESC9 D3DSURFACE_DESC
#endif
#ifndef D3DPRESENT_PARAMETERS9
#define D3DPRESENT_PARAMETERS9 D3DPRESENT_PARAMETERS
#endif

// Shorthand typedefs that MGE-XE code uses
typedef IDirect3DVertexBuffer9 IDirect3DVertexBuffer;
typedef IDirect3DIndexBuffer9 IDirect3DIndexBuffer;
typedef IDirect3DSurface9 IDirect3DSurface;
typedef IDirect3DTexture9 IDirect3DTexture;
typedef IDirect3D9 IDirect3D;
typedef IDirect3DDevice9 IDirect3DDevice;

// RGBVECTOR — MGE-XE's custom color type
typedef struct RGBVECTOR {
public:
    RGBVECTOR() {}
    RGBVECTOR( DWORD rgb );
    RGBVECTOR( CONST FLOAT*);
    RGBVECTOR( CONST D3DXFLOAT16*);
    RGBVECTOR( CONST RGBVECTOR*);
    RGBVECTOR( CONST RGBVECTOR&);
    RGBVECTOR( CONST D3DCOLORVALUE&);
    RGBVECTOR( CONST D3DXCOLOR&);
    RGBVECTOR( FLOAT r, FLOAT g, FLOAT b );

    operator DWORD () const;
    operator FLOAT* ();
    operator CONST FLOAT* () const;

    RGBVECTOR& operator += (CONST RGBVECTOR&);
    RGBVECTOR& operator -= (CONST RGBVECTOR&);
    RGBVECTOR& operator += (CONST D3DXCOLOR&);
    RGBVECTOR& operator -= (CONST D3DXCOLOR&);
    RGBVECTOR& operator *= (FLOAT);
    RGBVECTOR& operator /= (FLOAT);

    RGBVECTOR operator + () const;
    RGBVECTOR operator - () const;

    RGBVECTOR operator + (CONST RGBVECTOR&) const;
    RGBVECTOR operator - (CONST RGBVECTOR&) const;
    RGBVECTOR operator + (CONST D3DXCOLOR&) const;
    RGBVECTOR operator - (CONST D3DXCOLOR&) const;
    RGBVECTOR operator * (FLOAT) const;
    RGBVECTOR operator / (FLOAT) const;

    friend RGBVECTOR operator * (FLOAT, CONST RGBVECTOR&);

    BOOL operator == (CONST RGBVECTOR&) const;
    BOOL operator != (CONST RGBVECTOR&) const;
    BOOL operator == (CONST D3DXCOLOR&) const;
    BOOL operator != (CONST D3DXCOLOR&) const;

    FLOAT r, g, b;
} RGBVECTOR, *LPRGBVECTOR;

// RGBVECTOR method definitions — use MGE-XE's inline implementations
#include "d3d8header.inl"
