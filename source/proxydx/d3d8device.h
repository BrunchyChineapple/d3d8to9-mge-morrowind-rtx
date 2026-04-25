/**
 * Shadow header for proxydx/d3d8device.h
 * MGE-XE's ProxyDevice is replaced by d3d8to9's Direct3DDevice8.
 * This header provides the necessary type aliases.
 */
#pragma once

#include "../d3d8to9.hpp"

// MGE-XE code that references ProxyDevice should use Direct3DDevice8 instead
typedef Direct3DDevice8 ProxyDevice;
typedef Direct3D8 ProxyD3D;
