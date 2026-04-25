/**
 * Shadow header for proxydx/direct3d8.h
 * MGE-XE's ProxyD3D is replaced by d3d8to9's Direct3D8.
 */
#pragma once

#include "../d3d8to9.hpp"

// Already typedef'd in d3d8device.h shadow, but guard for direct includes
#ifndef ProxyD3D
typedef Direct3D8 ProxyD3D;
#endif
