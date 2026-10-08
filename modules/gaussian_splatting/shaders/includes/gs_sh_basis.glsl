// Normalized real SH, Condon-Shortley phase, degree-major m=-l..l.
// Caller supplies a unit direction. Shared with native recurrence tests.
#ifndef GS_SH_BASIS_GLSL_INCLUDED
#define GS_SH_BASIS_GLSL_INCLUDED
#ifndef GS_SH_OUT
#define GS_SH_OUT out
#endif
const float SH_C0 = 0.28209479177387814;
const float SH_C1 = 0.4886025119029199;

void gs_compute_real_sh_basis(vec3 dir, uint max_band, GS_SH_OUT float basis[25]) {
    for (int i = 0; i < 25; i++) basis[i] = 0.0;
    float x = dir.x, y = dir.y, z = dir.z;
    basis[0] = SH_C0;
    if (max_band < 1u) return;
    basis[1] = -SH_C1 * y;
    basis[2] = SH_C1 * z;
    basis[3] = -SH_C1 * x;
    if (max_band < 2u) return;
    float xx = x*x, yy = y*y, zz = z*z;
    basis[4] = 1.0925484305920792 * x*y;
    basis[5] = -1.0925484305920792 * y*z;
    basis[6] = 0.31539156525252005 * (3.0*zz - 1.0);
    basis[7] = -1.0925484305920792 * x*z;
    basis[8] = 0.5462742152960396 * (xx - yy);
    if (max_band < 3u) return;
    basis[9] = -0.5900435899266435 * y*(3.0*xx - yy);
    basis[10] = 2.890611442640554 * x*y*z;
    basis[11] = -0.4570457994644658 * y*(5.0*zz - 1.0);
    basis[12] = 0.3731763325901154 * z*(5.0*zz - 3.0);
    basis[13] = -0.4570457994644658 * x*(5.0*zz - 1.0);
    basis[14] = 1.445305721320277 * z*(xx - yy);
    basis[15] = -0.5900435899266435 * x*(xx - 3.0*yy);
    if (max_band < 4u) return;
    basis[16] = 2.5033429417967046 * x*y*(xx - yy);
    basis[17] = -1.7701307697799304 * y*z*(3.0*xx - yy);
    basis[18] = 0.9461746957575601 * x*y*(7.0*zz - 1.0);
    basis[19] = -0.6690465435572892 * y*z*(7.0*zz - 3.0);
    basis[20] = 0.10578554691520431 * (zz*(35.0*zz - 30.0) + 3.0);
    basis[21] = -0.6690465435572892 * x*z*(7.0*zz - 3.0);
    basis[22] = 0.47308734787878004 * (xx - yy)*(7.0*zz - 1.0);
    basis[23] = -1.7701307697799304 * x*z*(xx - 3.0*yy);
    basis[24] = 0.6258357354491761 * (xx*xx - 6.0*xx*yy + yy*yy);
}
#endif
