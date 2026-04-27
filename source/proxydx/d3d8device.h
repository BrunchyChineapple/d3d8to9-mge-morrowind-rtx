/**
 * Shadow header for proxydx/d3d8device.h
 *
 * MGE-XE 0.18.0's ProxyDevice is replaced by d3d8to9's Direct3DDevice8.
 * This header provides the necessary type aliases so MGE-XE code that
 * references ProxyDevice resolves to Direct3DDevice8.
 */
#pragma once

#include "d3d8interface.h"

// ProxyDevice is already typedef'd in d3d8interface.h shadow.
// This file exists so that #include "proxydx/d3d8device.h" resolves.
