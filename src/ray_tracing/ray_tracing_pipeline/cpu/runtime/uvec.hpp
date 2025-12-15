#pragma once
namespace rt
{

    struct uvec2;
    struct uvec3;
    struct uvec4;

    template <int A, int B>
    struct uswizzle2
    {
        unsigned int v[4];
        inline operator uvec2() const;
        inline uswizzle2 &operator=(const uvec2 &rhs);
    };

    template <int A, int B, int C>
    struct uswizzle3
    {
        unsigned int v[4];
        inline operator uvec3() const;
        inline uswizzle3 &operator=(const uvec3 &rhs);
    };

    template <int A, int B, int C, int D>
    struct uswizzle4
    {
        unsigned int v[4];
        inline operator uvec4() const;
        inline uswizzle4 &operator=(const uvec4 &rhs);
    };

    struct uvec2
    {
        uvec2() : x(0), y(0) {}
        uvec2(unsigned int xVal, unsigned int yVal) : x(xVal), y(yVal) {}
        union
        {
            struct
            {
                unsigned int x, y;
            };
            float _pad;
            unsigned int data[2];
            uswizzle2<0, 1> xy;
            uswizzle2<1, 0> yx;
        };
    };

    struct uvec3
    {
        uvec3() : x(0), y(0), z(0), _pad(0) {}
        uvec3(unsigned int xVal, unsigned int yVal, unsigned int zVal) : x(xVal), y(yVal), z(zVal), _pad(0) {}
        union
        {
            struct
            {
                unsigned int x, y, z;
                unsigned int _pad;
            };
            unsigned int data[4];
            uswizzle2<0, 1> xy;
            uswizzle3<0, 1, 2> xyz;
        };
    };

    struct uvec4
    {
        uvec4() : x(0), y(0), z(0), w(0) {}
        uvec4(unsigned int xVal, unsigned int yVal, unsigned int zVal, unsigned int wVal) : x(xVal), y(yVal), z(zVal), w(wVal) {}
        union
        {
            struct
            {
                unsigned int x, y, z, w;
            };
            unsigned int data[4];
            uswizzle2<0, 1> xy;
            uswizzle3<0, 1, 2> xyz;
            uswizzle4<0, 1, 2, 3> xyzw;
        };
    };

    template <int A, int B>
    inline uswizzle2<A, B>::operator uvec2() const { return {v[A], v[B]}; }

    template <int A, int B>
    inline uswizzle2<A, B> &uswizzle2<A, B>::operator=(const uvec2 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        return *this;
    }

    template <int A, int B, int C>
    inline uswizzle3<A, B, C>::operator uvec3() const
    {
        return {v[A], v[B], v[C]};
    }

    template <int A, int B, int C>
    inline uswizzle3<A, B, C> &uswizzle3<A, B, C>::operator=(const uvec3 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        v[C] = rhs.z;
        return *this;
    }

    template <int A, int B, int C, int D>
    inline uswizzle4<A, B, C, D>::operator uvec4() const
    {
        return {v[A], v[B], v[C], v[D]};
    }

    template <int A, int B, int C, int D>
    inline uswizzle4<A, B, C, D> &uswizzle4<A, B, C, D>::operator=(const uvec4 &rhs)
    {
        v[A] = rhs.x;
        v[B] = rhs.y;
        v[C] = rhs.z;
        v[D] = rhs.w;
        return *this;
    }
}