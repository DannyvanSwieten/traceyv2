#pragma once

namespace rt
{
    struct ivec2;
    struct ivec3;
    struct ivec4;

    template <int A, int B>
    struct iswizzle2
    {
        int v[4];
        inline operator ivec2() const;
        inline iswizzle2 &operator=(const ivec2 &rhs);
    };

    template <int A, int B, int C>
    struct iswizzle3
    {
        int v[4];
        inline operator ivec3() const;
        inline iswizzle3 &operator=(const ivec3 &rhs);
    };

    template <int A, int B, int C, int D>
    struct iswizzle4
    {
        int v[4];
        inline operator ivec4() const;
        inline iswizzle4 &operator=(const ivec4 &rhs);
    };

    struct ivec2
    {
        ivec2() : x(0), y(0) {}
        ivec2(int xVal, int yVal) : x(xVal), y(yVal) {}
        union
        {
            struct
            {
                int x, y;
            };
            float _pad;
            int data[2];
            iswizzle2<0, 1> xy;
            iswizzle2<1, 0> yx;
        };
    };

    struct ivec3
    {
        ivec3() : x(0), y(0), z(0), _pad(0) {}
        ivec3(int xVal, int yVal, int zVal) : x(xVal), y(yVal), z(zVal), _pad(0) {}
        union
        {
            struct
            {
                int x, y, z;
                int _pad;
            };
            int data[4];
            iswizzle2<0, 1> xy;
            iswizzle3<0, 1, 2> xyz;
        };
    };

    struct ivec4
    {
        ivec4() : x(0), y(0), z(0), w(0) {}
        ivec4(int xVal, int yVal, int zVal, int wVal) : x(xVal), y(yVal), z(zVal), w(wVal) {}
        union
        {
            struct
            {
                int x, y, z, w;
            };
            int data[4];
            iswizzle2<0, 1> xy;
            iswizzle3<0, 1, 2> xyz;
            iswizzle4<0, 1, 2, 3> xyzw;
        };
    };

    template <int A, int B>
    inline iswizzle2<A, B>::operator ivec2() const { return {v[A], v[B]}; }

    template <int A, int B>
    inline iswizzle2<A, B> &iswizzle2<A, B>::operator=(const ivec2 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        return *this;
    }

    template <int A, int B, int C>
    inline iswizzle3<A, B, C>::operator ivec3() const
    {
        return {v[A], v[B], v[C]};
    }

    template <int A, int B, int C>
    inline iswizzle3<A, B, C> &iswizzle3<A, B, C>::operator=(const ivec3 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        v[C] = rhs.z;
        return *this;
    }

    template <int A, int B, int C, int D>
    inline iswizzle4<A, B, C, D>::operator ivec4() const
    {
        return {v[A], v[B], v[C], v[D]};
    }

    template <int A, int B, int C, int D>
    inline iswizzle4<A, B, C, D> &iswizzle4<A, B, C, D>::operator=(const ivec4 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        v[C] = rhs.z;
        v[D] = rhs.w;
        return *this;
    }
}