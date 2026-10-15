// Free camera around the game's own view: recovers the eye and axes from the game's world -> clip
// matrix, and builds a new world -> clip matrix after moving/turning, with the game's projection.
// Matrices are column-major float[16] for column vectors (the game's row-vector clip matrix read as
// a column-major array is exactly that). World is the game's: Z up, metres.
#pragma once
#include <cmath>
#include <cstring>

namespace pop_native
{
    class FreeCamera
    {
    public:
        explicit FreeCamera(const float clip[16])
        {
            std::memcpy(base_, clip, sizeof base_);
            // rows of the column-vector matrix: x, y, w as functions of the world point
            float r0[4], r1[4], r3[4];
            for (int c = 0; c < 4; ++c)
            {
                r0[c] = base_[c * 4 + 0];
                r1[c] = base_[c * 4 + 1];
                r3[c] = base_[c * 4 + 3];
            }
            // eye: x = y = w = 0 (Cramer's rule on the 3x3 system)
            const float a[3][3] = {{r0[0], r0[1], r0[2]}, {r1[0], r1[1], r1[2]}, {r3[0], r3[1], r3[2]}};
            const float b[3] = {-r0[3], -r1[3], -r3[3]};
            const float det = det3(a);
            for (int k = 0; k < 3; ++k)
            {
                float m[3][3];
                std::memcpy(m, a, sizeof m);
                for (int r = 0; r < 3; ++r)
                    m[r][k] = b[r];
                eye0_[k] = det != 0 ? det3(m) / det : 0;
            }
            normalise(r3, forward0_);
            orthoNormalise(r0, forward0_, right0_);
            float down[3];
            orthoNormalise(r1, forward0_, down);
            for (int k = 0; k < 3; ++k)
                up0_[k] = -down[k]; // GS screen y grows downwards
            reset();
        }

        void reset()
        {
            yaw_ = pitch_ = 0;
            std::memcpy(eye_, eye0_, sizeof eye_);
        }
        // radians: yaw about world Z, pitch about the camera's right axis
        void turn(float dYaw, float dPitch)
        {
            yaw_ += dYaw;
            pitch_ = std::fmax(-1.5f, std::fmin(1.5f, pitch_ + dPitch));
        }
        // metres along the current forward / right axes and world up
        void move(float forward, float right, float up)
        {
            float q[9], f[3], r[3];
            rotation(q);
            apply(q, forward0_, f);
            apply(q, right0_, r);
            for (int k = 0; k < 3; ++k)
                eye_[k] += f[k] * forward + r[k] * right;
            eye_[2] += up;
        }
        // world -> clip for the current camera: base * T(eye0) * Q^T * T(-eye)
        void clip(float out[16]) const
        {
            float q[9];
            rotation(q);
            float x[16] = {}; // X(p) = eye0 + Q^T (p - eye)
            for (int r = 0; r < 3; ++r)
            {
                float t = eye0_[r];
                for (int c = 0; c < 3; ++c)
                {
                    x[c * 4 + r] = q[c * 3 + r]; // (Q^T)[r][c] = Q[c][r]
                    t -= q[c * 3 + r] * eye_[c];
                }
                x[12 + r] = t;
            }
            x[15] = 1;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                {
                    float s = 0;
                    for (int k = 0; k < 4; ++k)
                        s += base_[k * 4 + r] * x[c * 4 + k];
                    out[c * 4 + r] = s;
                }
        }
        bool moved() const { return yaw_ != 0 || pitch_ != 0 || std::memcmp(eye_, eye0_, sizeof eye_) != 0; }

    private:
        static float det3(const float m[3][3])
        {
            return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        }
        static void normalise(const float v[3], float out[3])
        {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            for (int k = 0; k < 3; ++k)
                out[k] = l > 0 ? v[k] / l : 0;
        }
        static void orthoNormalise(const float v[3], const float n[3], float out[3])
        {
            const float d = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
            const float t[3] = {v[0] - d * n[0], v[1] - d * n[1], v[2] - d * n[2]};
            normalise(t, out);
        }
        // 3x3 rotation about a unit axis (Rodrigues), row-major q[r*3+c]
        static void axisAngle(const float a[3], float angle, float q[9])
        {
            const float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
            q[0] = t * a[0] * a[0] + c;        q[1] = t * a[0] * a[1] - s * a[2]; q[2] = t * a[0] * a[2] + s * a[1];
            q[3] = t * a[0] * a[1] + s * a[2]; q[4] = t * a[1] * a[1] + c;        q[5] = t * a[1] * a[2] - s * a[0];
            q[6] = t * a[0] * a[2] - s * a[1]; q[7] = t * a[1] * a[2] + s * a[0]; q[8] = t * a[2] * a[2] + c;
        }
        static void mul3(const float a[9], const float b[9], float out[9])
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
        }
        static void apply(const float q[9], const float v[3], float out[3])
        {
            for (int r = 0; r < 3; ++r)
                out[r] = q[r * 3] * v[0] + q[r * 3 + 1] * v[1] + q[r * 3 + 2] * v[2];
        }
        // Q: the new camera's orientation relative to the game's (world frame): yaw about Z, then
        // pitch about the original right axis
        void rotation(float q[9]) const
        {
            const float z[3] = {0, 0, 1};
            float y[9], p[9];
            axisAngle(z, yaw_, y);
            axisAngle(right0_, pitch_, p);
            mul3(y, p, q);
        }

        float base_[16];
        float eye0_[3], forward0_[3], right0_[3], up0_[3];
        float eye_[3];
        float yaw_ = 0, pitch_ = 0;
    };
}
