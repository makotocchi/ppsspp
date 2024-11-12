// Copyright (c) 2013- PPSSPP Project.

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

#include <mutex>
#include "Common/Common.h"
#include "Common/Data/Convert/ColorConv.h"
#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/Software/DrawPixel.h"
#include "GPU/Software/FuncId.h"
#include "GPU/Software/Rasterizer.h"
#include "GPU/Software/SoftGpu.h"

using namespace Math3D;

namespace Rasterizer {

std::mutex jitCacheLock;
PixelJitCache *jitCache = nullptr;

void Init() {
	jitCache = new PixelJitCache();
}

void Shutdown() {
	delete jitCache;
	jitCache = nullptr;
}

bool DescribeCodePtr(const u8 *ptr, std::string &name) {
	if (!jitCache->IsInSpace(ptr)) {
		return false;
	}

	name = jitCache->DescribeCodePtr(ptr);
	return true;
}

static inline u8 GetPixelStencil(GEBufferFormat fmt, int fbStride, int x, int y) {
	if (fmt == GE_FORMAT_565) {
		// Always treated as 0 for comparison purposes.
		return 0;
	} else if (fmt == GE_FORMAT_5551) {
		return ((fb.Get16(x, y, fbStride) & 0x8000) != 0) ? 0xFF : 0;
	} else if (fmt == GE_FORMAT_4444) {
		return Convert4To8(fb.Get16(x, y, fbStride) >> 12);
	} else {
		return fb.Get32(x, y, fbStride) >> 24;
	}
}

static inline void SetPixelStencil(GEBufferFormat fmt, int fbStride, uint32_t targetWriteMask, int x, int y, u8 value) {
	if (fmt == GE_FORMAT_565) {
		// Do nothing
	} else if (fmt == GE_FORMAT_5551) {
		if ((targetWriteMask & 0x8000) == 0) {
			u16 pixel = fb.Get16(x, y, fbStride) & ~0x8000;
			pixel |= (value & 0x80) << 8;
			fb.Set16(x, y, fbStride, pixel);
		}
	} else if (fmt == GE_FORMAT_4444) {
		const u16 write_mask = targetWriteMask | 0x0FFF;
		u16 pixel = fb.Get16(x, y, fbStride) & write_mask;
		pixel |= ((u16)value << 8) & ~write_mask;
		fb.Set16(x, y, fbStride, pixel);
	} else {
		const u32 write_mask = targetWriteMask | 0x00FFFFFF;
		u32 pixel = fb.Get32(x, y, fbStride) & write_mask;
		pixel |= ((u32)value << 24) & ~write_mask;
		fb.Set32(x, y, fbStride, pixel);
	}
}

static inline u16 GetPixelDepth(int x, int y, int stride) {
	return depthbuf.Get16(x, y, stride);
}

static inline void SetPixelDepth(int x, int y, int stride, u16 value) {
	depthbuf.Set16(x, y, stride, value);
}

// NOTE: These likely aren't endian safe
static inline u32 GetPixelColor(GEBufferFormat fmt, int fbStride, int x, int y) {
	switch (fmt) {
	case GE_FORMAT_565:
		// A should be zero for the purposes of alpha blending.
		return RGB565ToRGBA8888(fb.Get16(x, y, fbStride)) & 0x00FFFFFF;

	case GE_FORMAT_5551:
		return RGBA5551ToRGBA8888(fb.Get16(x, y, fbStride));

	case GE_FORMAT_4444:
		return RGBA4444ToRGBA8888(fb.Get16(x, y, fbStride));

	case GE_FORMAT_8888:
		return fb.Get32(x, y, fbStride);

	default:
		return 0;
	}
}

static inline void SetPixelColor(GEBufferFormat fmt, int fbStride, int x, int y, u32 value, u32 old_value, u32 targetWriteMask) {
	switch (fmt) {
	case GE_FORMAT_565:
		value = RGBA8888ToRGB565(value);
		if (targetWriteMask != 0) {
			old_value = RGBA8888ToRGB565(old_value);
			value = (value & ~targetWriteMask) | (old_value & targetWriteMask);
		}
		fb.Set16(x, y, fbStride, value);
		break;

	case GE_FORMAT_5551:
		value = RGBA8888ToRGBA5551(value);
		if (targetWriteMask != 0) {
			old_value = RGBA8888ToRGBA5551(old_value);
			value = (value & ~targetWriteMask) | (old_value & targetWriteMask);
		}
		fb.Set16(x, y, fbStride, value);
		break;

	case GE_FORMAT_4444:
		value = RGBA8888ToRGBA4444(value);
		if (targetWriteMask != 0) {
			old_value = RGBA8888ToRGBA4444(old_value);
			value = (value & ~targetWriteMask) | (old_value & targetWriteMask);
		}
		fb.Set16(x, y, fbStride, value);
		break;

	case GE_FORMAT_8888:
		value = (value & ~targetWriteMask) | (old_value & targetWriteMask);
		fb.Set32(x, y, fbStride, value);
		break;

	default:
		break;
	}
}

static inline bool AlphaTestPassed(const PixelFuncID &pixelID, int alpha) {
	const u8 ref = pixelID.alphaTestRef;
	if (pixelID.hasAlphaTestMask)
		alpha &= pixelID.cached.alphaTestMask;

	switch (pixelID.AlphaTestFunc()) {
	case GE_COMP_NEVER:
		return false;

	case GE_COMP_ALWAYS:
		return true;

	case GE_COMP_EQUAL:
		return (alpha == ref);

	case GE_COMP_NOTEQUAL:
		return (alpha != ref);

	case GE_COMP_LESS:
		return (alpha < ref);

	case GE_COMP_LEQUAL:
		return (alpha <= ref);

	case GE_COMP_GREATER:
		return (alpha > ref);

	case GE_COMP_GEQUAL:
		return (alpha >= ref);
	}
	return true;
}

static inline bool ColorTestPassed(const PixelFuncID &pixelID, const Vec3<int> &color) {
	const u32 mask = pixelID.cached.colorTestMask;
	const u32 c = color.ToRGB() & mask;
	const u32 ref = pixelID.cached.colorTestRef;
	switch (pixelID.cached.colorTestFunc) {
	case GE_COMP_NEVER:
		return false;

	case GE_COMP_ALWAYS:
		return true;

	case GE_COMP_EQUAL:
		return c == ref;

	case GE_COMP_NOTEQUAL:
		return c != ref;

	default:
		return true;
	}
}

static inline bool StencilTestPassed(const PixelFuncID &pixelID, u8 stencil) {
	if (pixelID.hasStencilTestMask)
		stencil &= pixelID.cached.stencilTestMask;
	u8 ref = pixelID.stencilTestRef;
	switch (pixelID.StencilTestFunc()) {
	case GE_COMP_NEVER:
		return false;

	case GE_COMP_ALWAYS:
		return true;

	case GE_COMP_EQUAL:
		return ref == stencil;

	case GE_COMP_NOTEQUAL:
		return ref != stencil;

	case GE_COMP_LESS:
		return ref < stencil;

	case GE_COMP_LEQUAL:
		return ref <= stencil;

	case GE_COMP_GREATER:
		return ref > stencil;

	case GE_COMP_GEQUAL:
		return ref >= stencil;
	}
	return true;
}

static inline u8 ApplyStencilOp(GEBufferFormat fmt, uint8_t stencilReplace, GEStencilOp op, u8 old_stencil) {
	switch (op) {
	case GE_STENCILOP_KEEP:
		return old_stencil;

	case GE_STENCILOP_ZERO:
		return 0;

	case GE_STENCILOP_REPLACE:
		return stencilReplace;

	case GE_STENCILOP_INVERT:
		return ~old_stencil;

	case GE_STENCILOP_INCR:
		switch (fmt) {
		case GE_FORMAT_8888:
			if (old_stencil != 0xFF) {
				return old_stencil + 1;
			}
			return old_stencil;
		case GE_FORMAT_5551:
			return 0xFF;
		case GE_FORMAT_4444:
			if (old_stencil < 0xF0) {
				return old_stencil + 0x10;
			}
			return old_stencil;
		default:
			return old_stencil;
		}
		break;

	case GE_STENCILOP_DECR:
		switch (fmt) {
		case GE_FORMAT_4444:
			if (old_stencil >= 0x10)
				return old_stencil - 0x10;
			break;
		case GE_FORMAT_5551:
			return 0;
		default:
			if (old_stencil != 0)
				return old_stencil - 1;
			return old_stencil;
		}
		break;
	}

	return old_stencil;
}

static inline bool DepthTestPassed(GEComparison func, int x, int y, int stride, u16 z) {
	u16 reference_z = GetPixelDepth(x, y, stride);

	switch (func) {
	case GE_COMP_NEVER:
		return false;

	case GE_COMP_ALWAYS:
		return true;

	case GE_COMP_EQUAL:
		return (z == reference_z);

	case GE_COMP_NOTEQUAL:
		return (z != reference_z);

	case GE_COMP_LESS:
		return (z < reference_z);

	case GE_COMP_LEQUAL:
		return (z <= reference_z);

	case GE_COMP_GREATER:
		return (z > reference_z);

	case GE_COMP_GEQUAL:
		return (z >= reference_z);

	default:
		return 0;
	}
}

static inline u32 ApplyLogicOp(GELogicOp op, u32 old_color, u32 new_color) {
	// All of the operations here intentionally preserve alpha/stencil.
	switch (op) {
	case GE_LOGIC_CLEAR:
		new_color &= 0xFF000000;
		break;

	case GE_LOGIC_AND:
		new_color = new_color & (old_color | 0xFF000000);
		break;

	case GE_LOGIC_AND_REVERSE:
		new_color = new_color & (~old_color | 0xFF000000);
		break;

	case GE_LOGIC_COPY:
		// No change to new_color.
		break;

	case GE_LOGIC_AND_INVERTED:
		new_color = (~new_color & (old_color & 0x00FFFFFF)) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_NOOP:
		new_color = (old_color & 0x00FFFFFF) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_XOR:
		new_color = new_color ^ (old_color & 0x00FFFFFF);
		break;

	case GE_LOGIC_OR:
		new_color = new_color | (old_color & 0x00FFFFFF);
		break;

	case GE_LOGIC_NOR:
		new_color = (~(new_color | old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_EQUIV:
		new_color = (~(new_color ^ old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_INVERTED:
		new_color = (~old_color & 0x00FFFFFF) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_OR_REVERSE:
		new_color = new_color | (~old_color & 0x00FFFFFF);
		break;

	case GE_LOGIC_COPY_INVERTED:
		new_color = (~new_color & 0x00FFFFFF) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_OR_INVERTED:
		new_color = ((~new_color | old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_NAND:
		new_color = (~(new_color & old_color) & 0x00FFFFFF) | (new_color & 0xFF000000);
		break;

	case GE_LOGIC_SET:
		new_color |= 0x00FFFFFF;
		break;
	}

	return new_color;
}

template <bool clearMode, GEBufferFormat fbFormat>
void SOFTRAST_CALL DrawSinglePixel(int x, int y, int z, int fog, Vec4IntArg color_in, const PixelFuncID &pixelID) {
	Vec4<int> prim_color = Vec4<int>(color_in).Clamp(0, 255);
	// Depth range test - applied in clear mode, if not through mode.
	if (pixelID.applyDepthRange)
		if (z < pixelID.cached.minz || z > pixelID.cached.maxz)
			return;

	if (pixelID.AlphaTestFunc() != GE_COMP_ALWAYS && !clearMode)
		if (!AlphaTestPassed(pixelID, prim_color.a()))
			return;

	// Fog is applied prior to color test.
	if (pixelID.applyFog && !clearMode) {
		Vec3<int> fogColor = Vec3<int>::FromRGB(pixelID.cached.fogColor);
		fogColor = (prim_color.rgb() * fog + fogColor * (255 - fog)) / 255;
		prim_color.r() = fogColor.r();
		prim_color.g() = fogColor.g();
		prim_color.b() = fogColor.b();
	}

	if (pixelID.colorTest && !clearMode)
		if (!ColorTestPassed(pixelID, prim_color.rgb()))
			return;

	// In clear mode, it uses the alpha color as stencil.
	uint32_t targetWriteMask = pixelID.applyColorWriteMask ? pixelID.cached.colorWriteMask : 0;
	u8 stencil = clearMode ? prim_color.a() : GetPixelStencil(fbFormat, pixelID.cached.framebufStride, x, y);
	if (clearMode) {
		if (pixelID.DepthClear())
			SetPixelDepth(x, y, pixelID.cached.depthbufStride, z);
	} else if (pixelID.stencilTest) {
		const uint8_t stencilReplace = pixelID.hasStencilTestMask ? pixelID.cached.stencilRef : pixelID.stencilTestRef;
		if (!StencilTestPassed(pixelID, stencil)) {
			stencil = ApplyStencilOp(fbFormat, stencilReplace, pixelID.SFail(), stencil);
			SetPixelStencil(fbFormat, pixelID.cached.framebufStride, targetWriteMask, x, y, stencil);
			return;
		}

		// Also apply depth at the same time.  If disabled, same as passing.
		if (pixelID.DepthTestFunc() != GE_COMP_ALWAYS && !DepthTestPassed(pixelID.DepthTestFunc(), x, y, pixelID.cached.depthbufStride, z)) {
			stencil = ApplyStencilOp(fbFormat, stencilReplace, pixelID.ZFail(), stencil);
			SetPixelStencil(fbFormat, pixelID.cached.framebufStride, targetWriteMask, x, y, stencil);
			return;
		}

		stencil = ApplyStencilOp(fbFormat, stencilReplace, pixelID.ZPass(), stencil);
	} else {
		if (pixelID.DepthTestFunc() != GE_COMP_ALWAYS && !DepthTestPassed(pixelID.DepthTestFunc(), x, y, pixelID.cached.depthbufStride, z)) {
			return;
		}
	}

	if (pixelID.depthWrite && !clearMode)
		SetPixelDepth(x, y, pixelID.cached.depthbufStride, z);

	const u32 old_color = GetPixelColor(fbFormat, pixelID.cached.framebufStride, x, y);
	u32 new_color;

	// Dithering happens before the logic op and regardless of framebuffer format or clear mode.
	// We do it while alpha blending because it happens before clamping.
	if (pixelID.alphaBlend && !clearMode) {
		const Vec4<int> dst = Vec4<int>::FromRGBA(old_color);
		Vec3<int> blended = AlphaBlendingResult(pixelID, prim_color, dst);
		if (pixelID.dithering) {
			blended += Vec3<int>::AssignToAll(pixelID.cached.ditherMatrix[(y & 3) * 4 + (x & 3)]);
		}

		// ToRGB() always automatically clamps.
		new_color = blended.ToRGB();
		new_color |= stencil << 24;
	} else {
		if (pixelID.dithering) {
			// We'll discard alpha anyway.
			prim_color += Vec4<int>::AssignToAll(pixelID.cached.ditherMatrix[(y & 3) * 4 + (x & 3)]);
		}

#if defined(_M_SSE)
		new_color = Vec3<int>(prim_color.ivec).ToRGB();
		new_color |= stencil << 24;
#else
		new_color = Vec4<int>(prim_color.r(), prim_color.g(), prim_color.b(), stencil).ToRGBA();
#endif
	}

	// Logic ops are applied after blending (if blending is enabled.)
	if (pixelID.applyLogicOp && !clearMode) {
		// Logic ops don't affect stencil, which happens inside ApplyLogicOp.
		new_color = ApplyLogicOp(pixelID.cached.logicOp, old_color, new_color);
	}

	if (clearMode) {
		if (!pixelID.ColorClear())
			new_color = (new_color & 0xFF000000) | (old_color & 0x00FFFFFF);
		if (!pixelID.StencilClear())
			new_color = (new_color & 0x00FFFFFF) | (old_color & 0xFF000000);
	}

	SetPixelColor(fbFormat, pixelID.cached.framebufStride, x, y, new_color, old_color, targetWriteMask);
}

SingleFunc GetSingleFunc(const PixelFuncID &id) {
	SingleFunc jitted = jitCache->GetSingle(id);
	if (jitted) {
		return jitted;
	}

	return jitCache->GenericSingle(id);
}

SingleFunc PixelJitCache::GenericSingle(const PixelFuncID &id) {
	if (id.clearMode) {
		switch (id.fbFormat) {
		case GE_FORMAT_565:
			return &DrawSinglePixel<true, GE_FORMAT_565>;
		case GE_FORMAT_5551:
			return &DrawSinglePixel<true, GE_FORMAT_5551>;
		case GE_FORMAT_4444:
			return &DrawSinglePixel<true, GE_FORMAT_4444>;
		case GE_FORMAT_8888:
			return &DrawSinglePixel<true, GE_FORMAT_8888>;
		}
	}
	switch (id.fbFormat) {
	case GE_FORMAT_565:
		return &DrawSinglePixel<false, GE_FORMAT_565>;
	case GE_FORMAT_5551:
		return &DrawSinglePixel<false, GE_FORMAT_5551>;
	case GE_FORMAT_4444:
		return &DrawSinglePixel<false, GE_FORMAT_4444>;
	case GE_FORMAT_8888:
		return &DrawSinglePixel<false, GE_FORMAT_8888>;
	}
	_assert_(false);
	return nullptr;
}

// 256k should be plenty of space for plenty of variations.
PixelJitCache::PixelJitCache() : CodeBlock(1024 * 64 * 4) {
}

void PixelJitCache::Clear() {
	CodeBlock::Clear();
	cache_.clear();
	addresses_.clear();

	constBlendHalf_11_4s_ = nullptr;
	constBlendInvert_11_4s_ = nullptr;
	const255_16s_ = nullptr;
	constBy255i_ = nullptr;
}

std::string PixelJitCache::DescribeCodePtr(const u8 *ptr) {
	constexpr bool USE_IDS = false;
	ptrdiff_t dist = 0x7FFFFFFF;
	if (USE_IDS) {
		PixelFuncID found{};
		for (const auto &it : addresses_) {
			ptrdiff_t it_dist = ptr - it.second;
			if (it_dist >= 0 && it_dist < dist) {
				found = it.first;
				dist = it_dist;
			}
		}

		return DescribePixelFuncID(found);
	}

	return CodeBlock::DescribeCodePtr(ptr);
}

SingleFunc PixelJitCache::GetSingle(const PixelFuncID &id) {
	std::lock_guard<std::mutex> guard(jitCacheLock);

	auto it = cache_.find(id);
	if (it != cache_.end()) {
		return it->second;
	}

	// x64 is typically 200-500 bytes, but let's be safe.
	if (GetSpaceLeft() < 65536) {
		Clear();
	}

#if PPSSPP_ARCH(AMD64) && !PPSSPP_PLATFORM(UWP)
	if (g_Config.bSoftwareRenderingJit) {
		addresses_[id] = GetCodePointer();
		SingleFunc func = CompileSingle(id);
		cache_[id] = func;
		return func;
	}
#endif
	return nullptr;
}

void ComputePixelBlendState(PixelBlendState &state, const PixelFuncID &id) {
	switch (id.AlphaBlendEq()) {
	case GE_BLENDMODE_MUL_AND_ADD:
	case GE_BLENDMODE_MUL_AND_SUBTRACT:
	case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE:
		state.usesFactors = true;
		break;

	case GE_BLENDMODE_MIN:
	case GE_BLENDMODE_MAX:
	case GE_BLENDMODE_ABSDIFF:
		break;
	}

	if (state.usesFactors) {
		switch (id.AlphaBlendSrc()) {
		case PixelBlendFactor::DSTALPHA:
		case PixelBlendFactor::INVDSTALPHA:
		case PixelBlendFactor::DOUBLEDSTALPHA:
		case PixelBlendFactor::DOUBLEINVDSTALPHA:
			state.usesDstAlpha = true;
			break;

		case PixelBlendFactor::OTHERCOLOR:
		case PixelBlendFactor::INVOTHERCOLOR:
			state.dstColorAsFactor = true;
			break;

		case PixelBlendFactor::SRCALPHA:
		case PixelBlendFactor::INVSRCALPHA:
		case PixelBlendFactor::DOUBLESRCALPHA:
		case PixelBlendFactor::DOUBLEINVSRCALPHA:
			state.srcColorAsFactor = true;
			break;

		default:
			break;
		}

		switch (id.AlphaBlendDst()) {
		case PixelBlendFactor::INVSRCALPHA:
			state.dstFactorIsInverse = id.AlphaBlendSrc() == PixelBlendFactor::SRCALPHA;
			state.srcColorAsFactor = true;
			break;

		case PixelBlendFactor::DOUBLEINVSRCALPHA:
			state.dstFactorIsInverse = id.AlphaBlendSrc() == PixelBlendFactor::DOUBLESRCALPHA;
			state.srcColorAsFactor = true;
			break;

		case PixelBlendFactor::DSTALPHA:
			state.usesDstAlpha = true;
			break;

		case PixelBlendFactor::INVDSTALPHA:
			state.dstFactorIsInverse = id.AlphaBlendSrc() == PixelBlendFactor::DSTALPHA;
			state.usesDstAlpha = true;
			break;

		case PixelBlendFactor::DOUBLEDSTALPHA:
			state.usesDstAlpha = true;
			break;

		case PixelBlendFactor::DOUBLEINVDSTALPHA:
			state.dstFactorIsInverse = id.AlphaBlendSrc() == PixelBlendFactor::DOUBLEDSTALPHA;
			state.usesDstAlpha = true;
			break;

		case PixelBlendFactor::OTHERCOLOR:
		case PixelBlendFactor::INVOTHERCOLOR:
			state.srcColorAsFactor = true;
			break;

		case PixelBlendFactor::SRCALPHA:
		case PixelBlendFactor::DOUBLESRCALPHA:
			state.srcColorAsFactor = true;
			break;

		default:
			break;
		}

		state.dstColorAsFactor = state.dstColorAsFactor || state.usesDstAlpha;
	}
}

};
