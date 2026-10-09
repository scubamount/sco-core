#pragma once
// Engine-agnostic 64-bit spatial math: Vector3d, Quatd and Transform, header-only.
// No game offsets and no game memory: the host converts what it reads from the game into these.
//
//   sco::engine::Transform ship{ {100, 200, 10}, sco::engine::Quatd::FromAxisAngle({0, 0, 1}, angle), 1 };
//   sco::engine::Vector3d inCity = ship.TransformPoint({1, 2, 3});   // ship-local -> parent
//
// Quatd stores (w, x, y, z) and is used as a unit rotation quaternion. The game stores rotations
// the other way round: the double rot[4] sc-offline's spawner reads and writes is (x, y, z, w).
// Convert at that boundary with FromXYZW / ToXYZW, never by casting the array.
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace sco::engine {

struct Vector3d {
    double x = 0, y = 0, z = 0;

    constexpr Vector3d operator+(const Vector3d& o) const { return { x + o.x, y + o.y, z + o.z }; }
    constexpr Vector3d operator-(const Vector3d& o) const { return { x - o.x, y - o.y, z - o.z }; }
    constexpr Vector3d operator-() const { return { -x, -y, -z }; }
    constexpr Vector3d operator*(double s) const { return { x * s, y * s, z * s }; }
    constexpr Vector3d operator/(double s) const { return { x / s, y / s, z / s }; }

    constexpr double Dot(const Vector3d& o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vector3d Cross(const Vector3d& o) const {
        return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x };
    }
    constexpr double LengthSquared() const { return Dot(*this); }
    double Length() const { return std::sqrt(LengthSquared()); }
    // The unit vector along this one; the zero vector when the length is under 1e-12.
    Vector3d Normalized() const {
        const double len = Length();
        if (len < 1e-12) return { 0, 0, 0 };
        return *this / len;
    }
    double DistanceTo(const Vector3d& o) const { return (*this - o).Length(); }
};

constexpr Vector3d operator*(double s, const Vector3d& v) { return v * s; }

struct Quatd {
    double w = 1, x = 0, y = 0, z = 0;   // default: identity

    static constexpr Quatd Identity() { return { 1, 0, 0, 0 }; }
    constexpr Quatd Conjugate() const { return { w, -x, -y, -z }; }
    // Hamilton product: (a * b).Rotate(v) == a.Rotate(b.Rotate(v)), b applied first.
    constexpr Quatd operator*(const Quatd& o) const {
        return { w * o.w - x * o.x - y * o.y - z * o.z,
                 w * o.x + x * o.w + y * o.z - z * o.y,
                 w * o.y - x * o.z + y * o.w + z * o.x,
                 w * o.z + x * o.y - y * o.x + z * o.w };
    }
    // Unit length; Identity() when the length is under 1e-12.
    Quatd Normalized() const {
        const double len = std::sqrt(w * w + x * x + y * y + z * z);
        if (len < 1e-12) return Identity();
        return { w / len, x / len, y / len, z / len };
    }
    // Rotates v by this (unit) quaternion: v + w*t + u x t with u = (x, y, z), t = 2 u x v.
    constexpr Vector3d Rotate(const Vector3d& v) const {
        const Vector3d u{ x, y, z };
        const Vector3d t = u.Cross(v) * 2.0;
        return v + t * w + u.Cross(t);
    }
    // The inverse rotation of a unit quaternion.
    constexpr Vector3d Unrotate(const Vector3d& v) const { return Conjugate().Rotate(v); }
    // Right-handed rotation by an angle in radians about axis (normalized here); Identity() for a
    // zero axis.
    static Quatd FromAxisAngle(const Vector3d& axis, double radians) {
        const Vector3d n = axis.Normalized();
        if (n.LengthSquared() == 0) return Identity();
        const double s = std::sin(radians * 0.5);
        return { std::cos(radians * 0.5), n.x * s, n.y * s, n.z * s };
    }
};

// The game's (x, y, z, w) order, as in the double rot[4] of sc-offline's spawner.
constexpr Quatd FromXYZW(const double q[4]) { return { q[3], q[0], q[1], q[2] }; }
constexpr void ToXYZW(const Quatd& q, double out[4]) {
    out[0] = q.x;
    out[1] = q.y;
    out[2] = q.z;
    out[3] = q.w;
}

// A frame relative to its parent: scale (uniform, non-zero), then rotation, then position.
struct Transform {
    Vector3d position;
    Quatd    rotation;
    double   scale = 1;

    // Local point -> parent frame.
    constexpr Vector3d TransformPoint(const Vector3d& local) const {
        return position + rotation.Rotate(local * scale);
    }
    // Parent-frame point -> local; the inverse of TransformPoint.
    constexpr Vector3d InverseTransformPoint(const Vector3d& parent) const {
        return rotation.Unrotate(parent - position) / scale;
    }
    // (a * b).TransformPoint(p) == a.TransformPoint(b.TransformPoint(p)): b is a child frame of a.
    constexpr Transform operator*(const Transform& child) const {
        return { TransformPoint(child.position), rotation * child.rotation, scale * child.scale };
    }
};

static_assert(std::is_standard_layout_v<Vector3d> && sizeof(Vector3d) == 24);
static_assert(std::is_standard_layout_v<Quatd> && sizeof(Quatd) == 32 && offsetof(Quatd, w) == 0);
static_assert(std::is_standard_layout_v<Transform>);

}  // namespace sco::engine
