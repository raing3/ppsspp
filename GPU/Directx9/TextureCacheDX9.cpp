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

#include <algorithm>
#include <cstring>

#include "Common/TimeUtil.h"
#include "Core/MemMap.h"
#include "Core/Reporting.h"
#include "GPU/ge_constants.h"

#include "GPU/GPUState.h"
#include "GPU/Directx9/TextureCacheDX9.h"
#include "GPU/Directx9/FramebufferManagerDX9.h"
#include "GPU/Directx9/ShaderManagerDX9.h"
#include "Common/GPU/D3D9/D3D9StateCache.h"
#include "GPU/Common/DepalettizeCommon.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/Common/TextureDecoder.h"
#include "Core/Config.h"
#include "Core/Host.h"

#include "ext/xxhash.h"
#include "Common/Math/math_util.h"

// NOTE: In the D3D backends, we flip R and B in the shaders, so while these look wrong, they're OK.

Draw::DataFormat FromD3D9Format(u32 fmt) {
	switch (fmt) {
	case D3DFMT_A8R8G8B8: default: return Draw::DataFormat::R8G8B8A8_UNORM;
	}
}

D3DFORMAT ToD3D9Format(Draw::DataFormat fmt) {
	switch (fmt) {
	case Draw::DataFormat::R8G8B8A8_UNORM: default: return D3DFMT_A8R8G8B8;
	}
}

namespace DX9 {

#define INVALID_TEX (LPDIRECT3DTEXTURE9)(-1)

static const D3DVERTEXELEMENT9 g_FramebufferVertexElements[] = {
	{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	D3DDECL_END()
};

TextureCacheDX9::TextureCacheDX9(Draw::DrawContext *draw)
	: TextureCacheCommon(draw) {
	lastBoundTexture = INVALID_TEX;
	isBgraBackend_ = true;

	device_ = (LPDIRECT3DDEVICE9)draw->GetNativeObject(Draw::NativeObject::DEVICE);
	deviceEx_ = (LPDIRECT3DDEVICE9EX)draw->GetNativeObject(Draw::NativeObject::DEVICE_EX);
	D3DCAPS9 pCaps;
	ZeroMemory(&pCaps, sizeof(pCaps));
	HRESULT result = 0;
	if (deviceEx_) {
		result = deviceEx_->GetDeviceCaps(&pCaps);
	} else {
		result = device_->GetDeviceCaps(&pCaps);
	}
	if (FAILED(result)) {
		WARN_LOG(G3D, "Failed to get the device caps!");
		maxAnisotropyLevel = 16;
	} else {
		maxAnisotropyLevel = pCaps.MaxAnisotropy;
	}

	nextTexture_ = nullptr;
	device_->CreateVertexDeclaration(g_FramebufferVertexElements, &pFramebufferVertexDecl);
}

TextureCacheDX9::~TextureCacheDX9() {
	pFramebufferVertexDecl->Release();
	Clear(true);
}

void TextureCacheDX9::SetFramebufferManager(FramebufferManagerDX9 *fbManager) {
	framebufferManager_ = fbManager;
}

void TextureCacheDX9::ReleaseTexture(TexCacheEntry *entry, bool delete_them) {
	LPDIRECT3DBASETEXTURE9 &texture = DxTex(entry);
	if (texture) {
		texture->Release();
		texture = nullptr;
	}
}

void TextureCacheDX9::InvalidateLastTexture() {
	lastBoundTexture = INVALID_TEX;
}

D3DFORMAT getClutDestFormat(GEPaletteFormat format) {
	switch (format) {
	case GE_CMODE_16BIT_ABGR4444:
		return D3DFMT_A4R4G4B4;
	case GE_CMODE_16BIT_ABGR5551:
		return D3DFMT_A1R5G5B5;
	case GE_CMODE_16BIT_BGR5650:
		return D3DFMT_R5G6B5;
	case GE_CMODE_32BIT_ABGR8888:
		return D3DFMT_A8R8G8B8;
	}
	// Should never be here !
	return D3DFMT_A8R8G8B8;
}

void TextureCacheDX9::ApplySamplingParams(const SamplerCacheKey &key) {
	D3DTEXTUREFILTERTYPE minFilt = (false ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR);
	dxstate.texMinFilter.set(key.minFilt ? minFilt : D3DTEXF_POINT);
	dxstate.texMipFilter.set(key.mipFilt ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	dxstate.texMagFilter.set(key.magFilt ? D3DTEXF_LINEAR : D3DTEXF_POINT);

	// DX9 mip levels are .. odd. The "max level" sets the LARGEST mip to use.
	// We can enforce only the top mip level by setting a massive negative lod bias.

	if (!key.mipEnable) {
		dxstate.texMaxMipLevel.set(0);
		dxstate.texMipLodBias.set(-100.0f);
	} else {
		dxstate.texMipLodBias.set((float)key.lodBias / 256.0f);
		dxstate.texMaxMipLevel.set(key.minLevel / 256);
	}

	dxstate.texAddressU.set(key.sClamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
	dxstate.texAddressV.set(key.tClamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
}

void TextureCacheDX9::StartFrame() {
	TextureCacheCommon::StartFrame();

	InvalidateLastTexture();
	timesInvalidatedAllThisFrame_ = 0;
	replacementTimeThisFrame_ = 0.0;

	if (texelsScaledThisFrame_) {
		VERBOSE_LOG(G3D, "Scaled %i texels", texelsScaledThisFrame_);
	}
	texelsScaledThisFrame_ = 0;
	if (clearCacheNextFrame_) {
		Clear(true);
		clearCacheNextFrame_ = false;
	} else {
		Decimate();
	}

	if (gstate_c.Supports(GPU_SUPPORTS_ANISOTROPY)) {
		DWORD aniso = 1 << g_Config.iAnisotropyLevel;
		DWORD anisotropyLevel = aniso > maxAnisotropyLevel ? maxAnisotropyLevel : aniso;
		device_->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, anisotropyLevel);
	}
}

void TextureCacheDX9::UpdateCurrentClut(GEPaletteFormat clutFormat, u32 clutBase, bool clutIndexIsSimple) {
	const u32 clutBaseBytes = clutBase * (clutFormat == GE_CMODE_32BIT_ABGR8888 ? sizeof(u32) : sizeof(u16));
	// Technically, these extra bytes weren't loaded, but hopefully it was loaded earlier.
	// If not, we're going to hash random data, which hopefully doesn't cause a performance issue.
	//
	// TODO: Actually, this seems like a hack.  The game can upload part of a CLUT and reference other data.
	// clutTotalBytes_ is the last amount uploaded.  We should hash clutMaxBytes_, but this will often hash
	// unrelated old entries for small palettes.
	// Adding clutBaseBytes may just be mitigating this for some usage patterns.
	const u32 clutExtendedBytes = std::min(clutTotalBytes_ + clutBaseBytes, clutMaxBytes_);

	if (replacer_.Enabled())
		clutHash_ = XXH32((const char *)clutBufRaw_, clutExtendedBytes, 0xC0108888);
	else
		clutHash_ = XXH3_64bits((const char *)clutBufRaw_, clutExtendedBytes) & 0xFFFFFFFF;
	clutBuf_ = clutBufRaw_;

	// Special optimization: fonts typically draw clut4 with just alpha values in a single color.
	clutAlphaLinear_ = false;
	clutAlphaLinearColor_ = 0;
	if (clutFormat == GE_CMODE_16BIT_ABGR4444 && clutIndexIsSimple) {
		const u16_le *clut = GetCurrentClut<u16_le>();
		clutAlphaLinear_ = true;
		clutAlphaLinearColor_ = clut[15] & 0x0FFF;
		for (int i = 0; i < 16; ++i) {
			u16 step = clutAlphaLinearColor_ | (i << 12);
			if (clut[i] != step) {
				clutAlphaLinear_ = false;
				break;
			}
		}
	}

	clutLastFormat_ = gstate.clutformat;
}

void TextureCacheDX9::BindTexture(TexCacheEntry *entry) {
	LPDIRECT3DBASETEXTURE9 texture = DxTex(entry);
	if (texture != lastBoundTexture) {
		device_->SetTexture(0, texture);
		lastBoundTexture = texture;
	}
	int maxLevel = (entry->status & TexCacheEntry::STATUS_NO_MIPS) ? 0 : entry->maxLevel;
	SamplerCacheKey samplerKey = GetSamplingParams(maxLevel, entry);
	ApplySamplingParams(samplerKey);
}

void TextureCacheDX9::Unbind() {
	device_->SetTexture(0, NULL);
	InvalidateLastTexture();
}

void TextureCacheDX9::BindAsClutTexture(Draw::Texture *tex) {
	LPDIRECT3DBASETEXTURE9 clutTexture = (LPDIRECT3DBASETEXTURE9)draw_->GetNativeObject(Draw::NativeObject::TEXTURE_VIEW, tex);
	device_->SetTexture(1, clutTexture);
	device_->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	device_->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	device_->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
}

void TextureCacheDX9::BuildTexture(TexCacheEntry *const entry) {
	BuildTexturePlan plan;
	if (!PrepareBuildTexture(plan, entry)) {
		// We're screwed?
		return;
	}

	int tw = plan.w;
	int th = plan.h;

	D3DFORMAT dstFmt = GetDestFormat(GETextureFormat(entry->format), gstate.getClutPaletteFormat());
	if (plan.replaced->GetSize(plan.baseLevelSrc, tw, th)) {
		dstFmt = ToD3D9Format(plan.replaced->Format(plan.baseLevelSrc));
	} else if (plan.scaleFactor > 1) {
		tw *= plan.scaleFactor;
		th *= plan.scaleFactor;
		dstFmt = D3DFMT_A8R8G8B8;
	}

	// We don't yet have mip generation, so clamp the number of levels to the ones we can load directly.
	int levels = std::min(plan.levelsToCreate, plan.levelsToLoad);

	LPDIRECT3DBASETEXTURE9 &texture = DxTex(entry);
	D3DPOOL pool = D3DPOOL_DEFAULT;
	int usage = D3DUSAGE_DYNAMIC;

	HRESULT hr;
	if (plan.depth == 1) {
		LPDIRECT3DTEXTURE9 tex;
		hr = device_->CreateTexture(tw, th, levels, usage, dstFmt, pool, &tex, nullptr);
		texture = tex;
	} else {
		LPDIRECT3DVOLUMETEXTURE9 tex;
		hr = device_->CreateVolumeTexture(tw, th, plan.depth, 1, usage, dstFmt, pool, &tex, nullptr);
		texture = tex;
	}

	if (FAILED(hr)) {
		INFO_LOG(G3D, "Failed to create D3D texture: %dx%d", tw, th);
		ReleaseTexture(entry, true);
		return;
	}

	if (!texture) {
		// What to do here?
		return;
	}

	Draw::DataFormat texFmt = FromD3D9Format(dstFmt);

	if (plan.depth == 1) {
		// Regular loop.
		for (int i = 0; i < levels; i++) {
			int dstLevel = i;
			HRESULT result;
			uint32_t lockFlag = dstLevel == 0 ? D3DLOCK_DISCARD : 0;  // Can only discard the top level
			D3DLOCKED_RECT rect{};

			result = ((LPDIRECT3DTEXTURE9)texture)->LockRect(dstLevel, &rect, NULL, lockFlag);
			if (FAILED(result)) {
				ERROR_LOG(G3D, "Failed to lock D3D 2D texture at level %d: %dx%d", i, plan.w, plan.h);
				return;
			}
			uint8_t *data = (uint8_t *)rect.pBits;
			int stride = rect.Pitch;
			LoadTextureLevel(*entry, data, stride, *plan.replaced, (i == 0) ? plan.baseLevelSrc : i, plan.scaleFactor, texFmt, false);
			((LPDIRECT3DTEXTURE9)texture)->UnlockRect(dstLevel);
		}
	} else {
		// 3D loop.
		D3DLOCKED_BOX box;
		HRESULT result = ((LPDIRECT3DVOLUMETEXTURE9)texture)->LockBox(0, &box, nullptr, D3DLOCK_DISCARD);
		if (FAILED(result)) {
			ERROR_LOG(G3D, "Failed to lock D3D 2D texture: %dx%dx%d", plan.w, plan.h, plan.depth);
			return;
		}

		uint8_t *data = (uint8_t *)box.pBits;
		int stride = box.RowPitch;
		for (int i = 0; i < plan.depth; i++) {
			LoadTextureLevel(*entry, data, stride, *plan.replaced, (i == 0) ? plan.baseLevelSrc : i, plan.scaleFactor, texFmt, false);
			data += box.SlicePitch;
		}
		((LPDIRECT3DVOLUMETEXTURE9)texture)->UnlockBox(0);
	}

	// Signal that we support depth textures so use it as one.
	if (plan.depth > 1) {
		entry->status |= TexCacheEntry::STATUS_3D;
	}

	if (plan.replaced->Valid()) {
		entry->SetAlphaStatus(TexCacheEntry::TexStatus(plan.replaced->AlphaStatus()));
	}
}

D3DFORMAT TextureCacheDX9::GetDestFormat(GETextureFormat format, GEPaletteFormat clutFormat) const {
	switch (format) {
	case GE_TFMT_CLUT4:
	case GE_TFMT_CLUT8:
	case GE_TFMT_CLUT16:
	case GE_TFMT_CLUT32:
		return getClutDestFormat(clutFormat);
	case GE_TFMT_4444:
		return D3DFMT_A4R4G4B4;
	case GE_TFMT_5551:
		return D3DFMT_A1R5G5B5;
	case GE_TFMT_5650:
		return D3DFMT_R5G6B5;
	case GE_TFMT_8888:
	case GE_TFMT_DXT1:
	case GE_TFMT_DXT3:
	case GE_TFMT_DXT5:
	default:
		return D3DFMT_A8R8G8B8;
	}
}

CheckAlphaResult TextureCacheDX9::CheckAlpha(const u32 *pixelData, u32 dstFmt, int w) {
	switch (dstFmt) {
	case D3DFMT_A4R4G4B4:
		return CheckAlpha16((const u16 *)pixelData, w, 0xF000);
	case D3DFMT_A1R5G5B5:
		return CheckAlpha16((const u16 *)pixelData, w, 0x8000);
	case D3DFMT_R5G6B5:
		// Never has any alpha.
		return CHECKALPHA_FULL;
	default:
		return CheckAlpha32(pixelData, w, 0xFF000000);
	}
}

bool TextureCacheDX9::GetCurrentTextureDebug(GPUDebugBuffer &buffer, int level) {
	SetTexture();
	ApplyTexture();

	LPDIRECT3DBASETEXTURE9 baseTex;
	LPDIRECT3DTEXTURE9 tex;
	LPDIRECT3DSURFACE9 offscreen = nullptr;
	HRESULT hr;

	bool success = false;
	hr = device_->GetTexture(0, &baseTex);
	if (SUCCEEDED(hr) && baseTex != NULL) {
		hr = baseTex->QueryInterface(IID_IDirect3DTexture9, (void **)&tex);
		if (SUCCEEDED(hr)) {
			D3DSURFACE_DESC desc;
			D3DLOCKED_RECT locked;
			tex->GetLevelDesc(level, &desc);
			RECT rect = { 0, 0, (LONG)desc.Width, (LONG)desc.Height };
			hr = tex->LockRect(level, &locked, &rect, D3DLOCK_READONLY);

			// If it fails, this means it's a render-to-texture, so we have to get creative.
			if (FAILED(hr)) {
				LPDIRECT3DSURFACE9 renderTarget = nullptr;
				hr = tex->GetSurfaceLevel(level, &renderTarget);
				if (renderTarget && SUCCEEDED(hr)) {
					hr = device_->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &offscreen, NULL);
					if (SUCCEEDED(hr)) {
						hr = device_->GetRenderTargetData(renderTarget, offscreen);
						if (SUCCEEDED(hr)) {
							hr = offscreen->LockRect(&locked, &rect, D3DLOCK_READONLY);
						}
					}
					renderTarget->Release();
				}
			}

			if (SUCCEEDED(hr)) {
				GPUDebugBufferFormat fmt;
				int pixelSize;
				switch (desc.Format) {
				case D3DFMT_A1R5G5B5:
					fmt = gstate_c.bgraTexture ? GPU_DBG_FORMAT_5551 : GPU_DBG_FORMAT_5551_BGRA;
					pixelSize = 2;
					break;
				case D3DFMT_A4R4G4B4:
					fmt = gstate_c.bgraTexture ? GPU_DBG_FORMAT_4444 : GPU_DBG_FORMAT_4444_BGRA;
					pixelSize = 2;
					break;
				case D3DFMT_R5G6B5:
					fmt = gstate_c.bgraTexture ? GPU_DBG_FORMAT_565 : GPU_DBG_FORMAT_565_BGRA;
					pixelSize = 2;
					break;
				case D3DFMT_A8R8G8B8:
					fmt = gstate_c.bgraTexture ? GPU_DBG_FORMAT_8888 : GPU_DBG_FORMAT_8888_BGRA;
					pixelSize = 4;
					break;
				default:
					fmt = GPU_DBG_FORMAT_INVALID;
					break;
				}

				if (fmt != GPU_DBG_FORMAT_INVALID) {
					buffer.Allocate(locked.Pitch / pixelSize, desc.Height, fmt, false);
					memcpy(buffer.GetData(), locked.pBits, locked.Pitch * desc.Height);
					success = true;
				} else {
					success = false;
				}
				if (offscreen) {
					offscreen->UnlockRect();
					offscreen->Release();
				} else {
					tex->UnlockRect(level);
				}
			}
			tex->Release();
		}
		baseTex->Release();
	}

	return success;
}

};
