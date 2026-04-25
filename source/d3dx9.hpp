#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d9.h>

#ifdef D3DX9
// When D3DX9 is defined, use the real d3dx9 SDK headers and link to d3dx9.lib.
// The D3DX functions (D3DXAssembleShader, D3DXDisassembleShader, D3DXLoadSurfaceFromSurface)
// are declared in the SDK headers and resolved by the linker.
#include <d3dx9.h>

// d3d8to9 code uses these flags which are defined in the SDK headers already
#ifndef D3DXASM_DEBUG
#define D3DXASM_DEBUG 0x0001
#endif
#ifndef D3DXASM_SKIPVALIDATION
#define D3DXASM_SKIPVALIDATION  0x0010
#endif

#ifdef NDEBUG
#define D3DXASM_FLAGS  0
#else
#define D3DXASM_FLAGS D3DXASM_DEBUG
#endif

#else // !D3DX9
// When D3DX9 is NOT defined, use function pointers loaded at runtime from d3dx9_43.dll

#define D3DX_FILTER_NONE 1

#define D3DXASM_DEBUG 0x0001
#define D3DXASM_SKIPVALIDATION  0x0010

#ifdef NDEBUG
#define D3DXASM_FLAGS  0
#else
#define D3DXASM_FLAGS D3DXASM_DEBUG
#endif // NDEBUG

struct D3DXMACRO
{
	LPCSTR Name;
	LPCSTR Definition;
};

typedef interface ID3DXBuffer *LPD3DXBUFFER;
typedef interface ID3DXInclude *LPD3DXINCLUDE;

DECLARE_INTERFACE_(ID3DXBuffer, IUnknown)
{
	// IUnknown
	STDMETHOD(QueryInterface)(THIS_ REFIID iid, LPVOID *ppv) PURE;
	STDMETHOD_(ULONG, AddRef)(THIS) PURE;
	STDMETHOD_(ULONG, Release)(THIS) PURE;

	// ID3DXBuffer
	STDMETHOD_(LPVOID, GetBufferPointer)(THIS) PURE;
	STDMETHOD_(DWORD, GetBufferSize)(THIS) PURE;
};

typedef HRESULT(WINAPI *PFN_D3DXAssembleShader)(LPCSTR pSrcData, UINT SrcDataLen, const D3DXMACRO *pDefines, LPD3DXINCLUDE pInclude, DWORD Flags, LPD3DXBUFFER *ppShader, LPD3DXBUFFER *ppErrorMsgs);
typedef HRESULT(WINAPI *PFN_D3DXDisassembleShader)(const DWORD *pShader, BOOL EnableColorCode, LPCSTR pComments, LPD3DXBUFFER *ppDisassembly);
typedef HRESULT(WINAPI *PFN_D3DXLoadSurfaceFromSurface)(LPDIRECT3DSURFACE9 pDestSurface, const PALETTEENTRY *pDestPalette, const RECT *pDestRect, LPDIRECT3DSURFACE9 pSrcSurface, const PALETTEENTRY *pSrcPalette, const RECT *pSrcRect, DWORD Filter, D3DCOLOR ColorKey);

extern PFN_D3DXAssembleShader D3DXAssembleShader;
extern PFN_D3DXDisassembleShader D3DXDisassembleShader;
extern PFN_D3DXLoadSurfaceFromSurface D3DXLoadSurfaceFromSurface;

#endif // D3DX9
