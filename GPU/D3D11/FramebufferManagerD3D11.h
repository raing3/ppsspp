// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <d3d11.h>

// Keeps track of allocated FBOs.
// Also provides facilities for drawing and later converting raw
// pixel data.

#include "GPU/GPUCommon.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "Common/GPU/thin3d.h"

class TextureCacheD3D11;
class DrawEngineD3D11;
class ShaderManagerD3D11;

class FramebufferManagerD3D11 : public FramebufferManagerCommon {
public:
	FramebufferManagerD3D11(Draw::DrawContext *draw);

	void EndFrame();

private:
	void PackDepthbuffer(VirtualFramebuffer *vfb, int x, int y, int w, int h);

	ID3D11Device *device_;
	ID3D11DeviceContext *context_;
	D3D_FEATURE_LEVEL featureLevel_;
};
