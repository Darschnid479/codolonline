#pragma once

// Project Dragon — 3D Math Library (IW4 / Call of Duty Online Coordinate System: +X East/Fwd, +Y North/Left, +Z Up)

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dragon::math
{
	inline constexpr float PI = 3.14159265358979323846f;
	inline constexpr float DEG2RAD = PI / 180.0f;
	inline constexpr float RAD2DEG = 180.0f / PI;

	struct Vec2
	{
		float x = 0.0f, y = 0.0f;
	};

	struct Vec3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;

		Vec3 operator+(const Vec3& b) const { return { x + b.x, y + b.y, z + b.z }; }
		Vec3 operator-(const Vec3& b) const { return { x - b.x, y - b.y, z - b.z }; }
		Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
		Vec3 operator/(float s) const { return { x / s, y / s, z / s }; }
		Vec3 operator-() const { return { -x, -y, -z }; }
		Vec3& operator+=(const Vec3& b) { x += b.x; y += b.y; z += b.z; return *this; }
		Vec3& operator-=(const Vec3& b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
		Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
	};

	struct Vec4
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
	};

	inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline Vec3 cross(const Vec3& a, const Vec3& b)
	{
		return {
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x
		};
	}
	inline float length_sq(const Vec3& v) { return dot(v, v); }
	inline float length(const Vec3& v) { return std::sqrt(length_sq(v)); }
	inline Vec3 normalize(const Vec3& v)
	{
		const float len = length(v);
		return len > 1e-6f ? (v / len) : Vec3{ 0.0f, 0.0f, 1.0f };
	}
	inline Vec3 vmin(const Vec3& a, const Vec3& b)
	{
		return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
	}
	inline Vec3 vmax(const Vec3& a, const Vec3& b)
	{
		return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
	}

	struct AABB
	{
		Vec3 mins{ 1e30f, 1e30f, 1e30f };
		Vec3 maxs{ -1e30f, -1e30f, -1e30f };

		void expand(const Vec3& p)
		{
			mins = vmin(mins, p);
			maxs = vmax(maxs, p);
		}
		Vec3 center() const { return (mins + maxs) * 0.5f; }
		Vec3 half_size() const { return (maxs - mins) * 0.5f; }
		bool valid() const { return mins.x <= maxs.x && mins.y <= maxs.y && mins.z <= maxs.z; }

		static AABB from_mid_half(const Vec3& mid, const Vec3& half)
		{
			return { mid - half, mid + half };
		}
	};

	// Column-major 4x4 matrix (OpenGL / Vulkan clip-space convention)
	struct Mat4
	{
		float m[16] = {
			1,0,0,0,
			0,1,0,0,
			0,0,1,0,
			0,0,0,1
		};

		static Mat4 identity() { return Mat4{}; }

		static Mat4 multiply(const Mat4& a, const Mat4& b)
		{
			Mat4 r{};
			for (int c = 0; c < 4; ++c)
			{
				for (int row = 0; row < 4; ++row)
				{
					r.m[c * 4 + row] =
						a.m[0 * 4 + row] * b.m[c * 4 + 0] +
						a.m[1 * 4 + row] * b.m[c * 4 + 1] +
						a.m[2 * 4 + row] * b.m[c * 4 + 2] +
						a.m[3 * 4 + row] * b.m[c * 4 + 3];
				}
			}
			return r;
		}

		Vec4 transform(const Vec4& v) const
		{
			return {
				m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12] * v.w,
				m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13] * v.w,
				m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
				m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w
			};
		}

		static Mat4 perspective(float fov_y_deg, float aspect, float znear, float zfar)
		{
			const float f = 1.0f / std::tan(fov_y_deg * 0.5f * DEG2RAD);
			Mat4 r{};
			for (float& x : r.m) x = 0.0f;
			r.m[0] = f / aspect;
			r.m[5] = f;
			r.m[10] = (zfar + znear) / (znear - zfar);
			r.m[11] = -1.0f;
			r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
			return r;
		}

		static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up)
		{
			const Vec3 f = normalize(target - eye);
			const Vec3 s = normalize(cross(f, up));
			const Vec3 u = cross(s, f);
			Mat4 r = identity();
			r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;  r.m[12] = -dot(s, eye);
			r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;  r.m[13] = -dot(u, eye);
			r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = dot(f, eye);
			return r;
		}
	};

	struct Plane
	{
		Vec3 normal{ 0, 0, 1 };
		float dist = 0.0f;
		float evaluate(const Vec3& p) const { return dot(normal, p) + dist; }
	};

	struct Frustum
	{
		Plane planes[6]; // left, right, bottom, top, near, far

		static Frustum from_view_proj(const Mat4& vp)
		{
			Frustum f;
			auto make_plane = [](float a, float b, float c, float d) -> Plane {
				const float len = std::sqrt(a * a + b * b + c * c);
				const float inv = len > 1e-6f ? (1.0f / len) : 1.0f;
				return { { a * inv, b * inv, c * inv }, d * inv };
			};
			// Gribb-Hartmann extraction from column-major VP matrix
			f.planes[0] = make_plane(vp.m[3] + vp.m[0], vp.m[7] + vp.m[4], vp.m[11] + vp.m[8],  vp.m[15] + vp.m[12]);
			f.planes[1] = make_plane(vp.m[3] - vp.m[0], vp.m[7] - vp.m[4], vp.m[11] - vp.m[8],  vp.m[15] - vp.m[12]);
			f.planes[2] = make_plane(vp.m[3] + vp.m[1], vp.m[7] + vp.m[5], vp.m[11] + vp.m[9],  vp.m[15] + vp.m[13]);
			f.planes[3] = make_plane(vp.m[3] - vp.m[1], vp.m[7] - vp.m[5], vp.m[11] - vp.m[9],  vp.m[15] - vp.m[13]);
			f.planes[4] = make_plane(vp.m[3] + vp.m[2], vp.m[7] + vp.m[6], vp.m[11] + vp.m[10], vp.m[15] + vp.m[14]);
			f.planes[5] = make_plane(vp.m[3] - vp.m[2], vp.m[7] - vp.m[6], vp.m[11] - vp.m[10], vp.m[15] - vp.m[14]);
			return f;
		}

		bool intersects_aabb(const Vec3& center, const Vec3& half) const
		{
			for (const Plane& p : planes)
			{
				const float r = std::fabs(p.normal.x) * half.x +
				                std::fabs(p.normal.y) * half.y +
				                std::fabs(p.normal.z) * half.z;
				if (p.evaluate(center) < -r)
					return false;
			}
			return true;
		}
	};

	// Convert IW4/CODOL yaw (deg around Z, 0=+X, 90=+Y) and pitch (deg, +up/-down) to forward/right/up vectors
	inline void angles_to_basis(float yaw_deg, float pitch_deg, Vec3& forward, Vec3& right, Vec3& up)
	{
		const float cy = std::cos(yaw_deg * DEG2RAD);
		const float sy = std::sin(yaw_deg * DEG2RAD);
		const float cp = std::cos(pitch_deg * DEG2RAD);
		const float sp = std::sin(pitch_deg * DEG2RAD);
		forward = normalize({ cy * cp, sy * cp, sp });
		right = normalize(cross(forward, Vec3{ 0.0f, 0.0f, 1.0f }));
		up = cross(right, forward);
	}
}
