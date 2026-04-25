/**
 * Shadow header for proxydx/d3d8texture.h
 * MGE-XE's ProxyTexture is replaced by d3d8to9's Direct3DTexture8.
 */
#pragma once

#include "../d3d8to9.hpp"

// MGE-XE code that references ProxyTexture should use Direct3DTexture8 instead
typedef Direct3DTexture8 ProxyTexture;
