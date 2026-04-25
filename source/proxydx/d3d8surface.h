/**
 * Shadow header for proxydx/d3d8surface.h
 * MGE-XE's ProxySurface is replaced by d3d8to9's Direct3DSurface8.
 */
#pragma once

#include "../d3d8to9.hpp"

// MGE-XE code that references ProxySurface should use Direct3DSurface8 instead
typedef Direct3DSurface8 ProxySurface;
