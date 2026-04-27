/**
 * Shadow header for proxydx/d3d8interface.h
 *
 * The 0.18.0 d3d8interface.h declares COM interfaces (IDirect3DDevice8, etc.)
 * and forward-declares ProxySurface/ProxyTexture/ProxyDevice/ProxyD3D.
 * d3d8to9 already provides full implementations of these interfaces,
 * so we redirect to d3d8to9's definitions and typedef the Proxy names.
 */
#pragma once

#include "d3d8header.h"
#include "../d3d8to9.hpp"

// d3d8to9 provides full COM interface classes that match the IDirect3D*8
// interfaces declared in the original d3d8interface.h. The DECLARE_INTERFACE_
// macros in the original are not needed — d3d8to9's classes already implement
// the correct vtable layout.

// Forward-declare / typedef the Proxy* names to d3d8to9 types
typedef Direct3DSurface8 ProxySurface;
typedef Direct3DTexture8 ProxyTexture;
typedef Direct3DDevice8 ProxyDevice;
typedef Direct3D8 ProxyD3D;
