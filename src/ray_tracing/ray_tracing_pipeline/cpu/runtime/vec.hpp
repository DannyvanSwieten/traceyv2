#pragma once
#include <cmath>
namespace rt
{
    struct vec2;
    struct vec3;
    struct vec4;

    template <int A, int B>
    struct swizzle2
    {
        float v[2]; // shares storage with the owning vector (via union)

        inline operator vec2() const;
        inline swizzle2 &operator=(const vec2 &rhs);
    };

    template <int A, int B, int C>
    struct swizzle3
    {
        float v[3];

        inline operator vec3() const;
        inline swizzle3 &operator=(const vec3 &rhs);
    };

    template <int A, int B, int C, int D>
    struct swizzle4
    {
        float v[4];

        inline operator vec4() const;
        inline swizzle4 &operator=(const vec4 &rhs);
    };

    struct vec2
    {
        vec2() : x(0), y(0) {}
        vec2(float xVal, float yVal) : x(xVal), y(yVal) {}
        union
        {
            struct
            {
                float x, y;
            };
            struct
            {
                float r, g;
            };
            float data[2];

            // Common 2-component swizzles
            swizzle2<0, 1> xy;
            swizzle2<1, 0> yx;
            swizzle2<0, 0> xx;
            swizzle2<1, 1> yy;
        };
    };

    // vec3 is padded to 16 bytes (like many GPU ABIs) so swizzles can share a 4-float storage.
    struct vec3
    {
        vec3() : x(0), y(0), z(0) {}
        vec3(float v) : x(v), y(v), z(v) {}
        vec3(float xVal, float yVal, float zVal) : x(xVal), y(yVal), z(zVal) {}
        union
        {
            struct
            {
                float x, y, z;
            };
            struct
            {
                float r, g, b;
            };
            float data[3];

            // Common 2-component swizzles
            swizzle2<0, 1> xy;
            swizzle2<1, 0> yx;
            swizzle2<1, 2> yz;
            swizzle2<2, 1> zy;
            swizzle2<0, 2> xz;
            swizzle2<2, 0> zx;

            // Common 3-component swizzles
            swizzle3<0, 1, 2> xyz;
            swizzle3<2, 1, 0> zyx;
            swizzle3<0, 0, 0> xxx;
            swizzle3<1, 1, 1> yyy;
            swizzle3<2, 2, 2> zzz;

            // Color aliases
            swizzle3<0, 1, 2> rgb;
        };
    };

    struct vec4
    {
        vec4() : x(0), y(0), z(0), w(0) {}
        vec4(vec3 v3, float wVal) : x(v3.x), y(v3.y), z(v3.z), w(wVal) {}
        vec4(float xVal, float yVal, float zVal, float wVal) : x(xVal), y(yVal), z(zVal), w(wVal) {}
        union
        {
            struct
            {
                float x, y, z, w;
            };
            struct
            {
                float r, g, b, a;
            };
            float data[4];

            // Common 2-component swizzles
            swizzle2<0, 1> xy;
            swizzle2<1, 0> yx;
            swizzle2<2, 3> zw;
            swizzle2<3, 2> wz;

            // Common 3-component swizzles
            swizzle3<0, 1, 2> xyz;
            swizzle3<1, 2, 3> yzw;
            swizzle3<0, 1, 3> xyw;

            // Common 4-component swizzles
            swizzle4<0, 1, 2, 3> xyzw;

            // Color aliases
            swizzle3<0, 1, 2> rgb;
            swizzle4<0, 1, 2, 3> rgba;
        };
    };

    // --- swizzle implementations ---------------------------------

    template <int A, int B>
    inline swizzle2<A, B>::operator vec2() const
    {
        return vec2{{v[A], v[B]}};
    }

    template <int A, int B>
    inline swizzle2<A, B> &swizzle2<A, B>::operator=(const vec2 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        return *this;
    }

    template <int A, int B, int C>
    inline swizzle3<A, B, C>::operator vec3() const
    {
        vec3 out{};
        out.x = v[A];
        out.y = v[B];
        out.z = v[C];
        return out;
    }

    template <int A, int B, int C>
    inline swizzle3<A, B, C> &swizzle3<A, B, C>::operator=(const vec3 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        v[C] = rhs.z;
        return *this;
    }

    template <int A, int B, int C, int D>
    inline swizzle4<A, B, C, D>::operator vec4() const
    {
        vec4 out{};
        out.x = v[A];
        out.y = v[B];
        out.z = v[C];
        out.w = v[D];
        return out;
    }

    template <int A, int B, int C, int D>
    inline swizzle4<A, B, C, D> &swizzle4<A, B, C, D>::operator=(const vec4 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        v[C] = rhs.z;
        v[D] = rhs.w;
        return *this;
    }

    inline vec2 operator+(const vec2 &a, const vec2 &b)
    {
        return vec2{a.x + b.x, a.y + b.y};
    }

    inline vec3 operator+(const vec3 &a, const vec3 &b)
    {
        return vec3{a.x + b.x, a.y + b.y, a.z + b.z};
    }

    inline vec4 operator+(const vec4 &a, const vec4 &b)
    {
        return vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }

    inline vec3 operator-(const vec3 &a, const vec3 &b)
    {
        return vec3{a.x - b.x, a.y - b.y, a.z - b.z};
    }

    inline vec3 operator*(const vec3 &a, float b)
    {
        return vec3{a.x * b, a.y * b, a.z * b};
    }

    inline vec3 operator*(float a, const vec3 &b)
    {
        return vec3{a * b.x, a * b.y, a * b.z};
    }

    inline vec3 normalize(const vec3 &v)
    {
        float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return vec3{v.x / len, v.y / len, v.z / len};
    }

    inline vec3 cross(const vec3 &a, const vec3 &b)
    {
        return vec3{
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
    }

    inline float dot(const vec3 &a, const vec3 &b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
} // namespace rt