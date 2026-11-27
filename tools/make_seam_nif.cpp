// Builds the seam-glow effect models (Skyrim SE .nif) with nifly.
//
// The model is made for a chest that is 1 x 1 unit across (X and Y) and
// is stretched to each chest's real size by the plugin. Heights (Z) are in
// normal game units, centered on the seam (Z = 0).
//
// Parts:
//   Band   - a thin glowing strip all the way around the seam
//   Rays   - soft light rays rising from the seam on every side
//   Flares - horizontal streaks leaking out past the ends of the seam

#include "NifFile.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace nifly;

namespace
{
	constexpr float BAND_HEIGHT = 10.0f;   // band + its soft halo (game units)
	constexpr float RAY_HEIGHT = 28.0f;    // how tall the rays are (game units)
	constexpr float FLARE_HEIGHT = 7.0f;   // end flare thickness (game units)
	constexpr float FLARE_LENGTH = 0.22f;  // end flare length (share of chest length)
	constexpr float OUT = 0.004f;          // push rays slightly in front of the band
	constexpr float RAY_TILT = 0.14f;      // how far ray tops lean away from the chest (share of chest size)

	struct Mesh
	{
		std::vector<Vector3>  verts;
		std::vector<Vector2>  uvs;
		std::vector<Triangle> tris;

		// Adds a quad from 4 corners: bottom-left, bottom-right, top-right, top-left.
		void Quad(Vector3 bl, Vector3 br, Vector3 tr, Vector3 tl,
			Vector2 uvBL = { 0, 1 }, Vector2 uvBR = { 1, 1 }, Vector2 uvTR = { 1, 0 }, Vector2 uvTL = { 0, 0 })
		{
			auto i = static_cast<uint16_t>(verts.size());
			verts.insert(verts.end(), { bl, br, tr, tl });
			uvs.insert(uvs.end(), { uvBL, uvBR, uvTR, uvTL });
			tris.push_back({ i, static_cast<uint16_t>(i + 1), static_cast<uint16_t>(i + 2) });
			tris.push_back({ i, static_cast<uint16_t>(i + 2), static_cast<uint16_t>(i + 3) });
		}
	};

	// The four sides of the 1x1 box: a point on the side at position t (-0.5..0.5),
	// and which way is "outward".
	struct Side
	{
		Vector3 origin;  // center of the side
		Vector3 along;   // direction along the side
		Vector3 out;     // outward direction
		int     rays;    // how many rays on this side
	};

	const Side SIDES[4] = {
		{ { 0.0f, -0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 6 },   // front
		{ { 0.0f, 0.5f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 6 },    // back
		{ { 0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 3 },     // right
		{ { -0.5f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, 3 },  // left
	};

	Vector3 At(const Side& s, float t, float z, float outward = 0.0f)
	{
		return { s.origin.x + s.along.x * t + s.out.x * outward, s.origin.y + s.along.y * t + s.out.y * outward, z };
	}

	Mesh MakeBand()
	{
		Mesh m;
		const float h = BAND_HEIGHT * 0.5f;
		for (auto& s : SIDES) {
			m.Quad(At(s, -0.5f, -h), At(s, 0.5f, -h), At(s, 0.5f, h), At(s, -0.5f, h));
		}
		return m;
	}

	Mesh MakeRays()
	{
		Mesh m;
		for (auto& s : SIDES) {
			for (int i = 0; i < s.rays; ++i) {
				// Spread evenly along the side, leaning outward toward the ends like a fan.
				const float t = -0.5f + (i + 0.5f) / s.rays;
				const float baseHalf = 0.05f;
				const float topHalf = 0.12f;
				const float lean = t * 0.35f;
				const float z0 = -1.0f;
				const float z1 = RAY_HEIGHT * (0.75f + 0.25f * std::cos(i * 2.1f));  // vary heights a bit
				// Tops tilt away from the chest, so the rays rise into the air instead of lying on the lid.
				m.Quad(At(s, t - baseHalf, z0, OUT), At(s, t + baseHalf, z0, OUT),
					At(s, t + lean + topHalf, z1, OUT + RAY_TILT), At(s, t + lean - topHalf, z1, OUT + RAY_TILT));
			}
		}
		return m;
	}

	Mesh MakeFlares()
	{
		Mesh m;
		const float h = FLARE_HEIGHT * 0.5f;
		for (int si = 0; si < 2; ++si) {  // front and back sides
			auto& s = SIDES[si];
			// Right end: u goes 0 (at the corner) -> 1 (tip)
			m.Quad(At(s, 0.5f, -h, OUT), At(s, 0.5f + FLARE_LENGTH, -h, OUT),
				At(s, 0.5f + FLARE_LENGTH, h, OUT), At(s, 0.5f, h, OUT));
			// Left end: mirrored, so u is 0 at the corner too
			m.Quad(At(s, -0.5f - FLARE_LENGTH, -h, OUT), At(s, -0.5f, -h, OUT), At(s, -0.5f, h, OUT),
				At(s, -0.5f - FLARE_LENGTH, h, OUT), { 1, 1 }, { 0, 1 }, { 0, 0 }, { 1, 0 });
		}
		return m;
	}

	void AddPart(NifFile& nif, const std::string& name, const Mesh& mesh, const std::string& texture, Color4 color)
	{
		NiShape* shape = nif.CreateShapeFromData(name, &mesh.verts, &mesh.tris, &mesh.uvs, nullptr);

		// The plugin stretches this 1x1 model to the chest's size, so give it a
		// generous size for the game's "is it on screen?" check.
		BoundingSphere bounds;
		bounds.center = Vector3(0.0f, 0.0f, RAY_HEIGHT * 0.3f);
		bounds.radius = 80.0f;
		shape->SetBounds(bounds);

		// Swap the default lighting shader for a glowing "effect" shader.
		auto effect = std::make_unique<BSEffectShaderProperty>();
		effect->sourceTexture.get() = texture;
		effect->textureClampMode = 0;     // clamp, so gradients don't repeat
		effect->shaderFlags1 = 0x80000000;  // depth test on (hidden behind walls)
		effect->shaderFlags2 = 0x00000010;  // double-sided, no depth write
		effect->baseColor = color;
		effect->baseColorScale = 2.0f;       // glow strength
		effect->falloffStartAngle = 1.0f;
		effect->falloffStopAngle = 1.0f;
		effect->falloffStartOpacity = 1.0f;
		effect->falloffStopOpacity = 1.0f;
		auto& hdr = nif.GetHeader();
		shape->ShaderPropertyRef()->index = hdr.AddBlock(std::move(effect));

		// Additive blending: light adds on top of what's behind it.
		auto alpha = std::make_unique<NiAlphaProperty>();
		alpha->flags = 1 | (6 << 1) | (0 << 5);  // blend on, source alpha, dest one
		alpha->threshold = 0;
		nif.AssignAlphaProperty(shape, std::move(alpha));
	}

	bool Build(const std::string& path, Color4 color)
	{
		NifFile nif;
		nif.Create(NiVersion::getSSE());
		nif.GetRootNode()->name.get() = "ChestGlowSeam";

		AddPart(nif, "Band", MakeBand(), "textures\\ChestGlow\\SeamBand.dds", color);
		AddPart(nif, "Rays", MakeRays(), "textures\\ChestGlow\\SeamRay.dds", color);
		AddPart(nif, "Flares", MakeFlares(), "textures\\ChestGlow\\SeamFlare.dds", color);

		nif.DeleteUnreferencedBlocks();  // drops the unused default shaders

		NifSaveOptions options;
		options.optimize = false;  // keep our generous bounds (optimizing would recalculate them)
		return nif.Save(path, options) == 0;
	}
}

int main(int argc, char** argv)
{
	const std::string dir = argc > 1 ? argv[1] : ".";
	bool ok = true;
	// One file per tier, with different starting colors (the plugin recolors them anyway).
	ok &= Build(dir + "/SeamLegendary.nif", Color4(1.0f, 0.78f, 0.0f, 1.0f));
	ok &= Build(dir + "/SeamEpic.nif", Color4(0.67f, 0.24f, 1.0f, 1.0f));
	ok &= Build(dir + "/SeamCommon.nif", Color4(0.24f, 0.86f, 0.31f, 1.0f));
	std::printf(ok ? "Saved 3 models.\n" : "FAILED to save.\n");
	return ok ? 0 : 1;
}
