/* Host test for main/rl/policy_core.c.
 *   host_policy <file.mpxp> <inputs.bin> <outputs.txt>
 * inputs.bin (float64, little-endian): n_steps, then per tick (4 per step,
 * plus one leading tick): dd[12] acc[3] gyro[3]; per step: cmd[3]; bias[3]
 * once at the start, offsets[12] once. outputs: per step obs[45] raw[12] dd[12]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "policy_core.h"

static double rd(FILE *f) { double v; if (fread(&v, 8, 1, f) != 1) { fprintf(stderr, "short input\n"); exit(2); } return v; }

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    FILE *fp = fopen(argv[1], "rb");
    fseek(fp, 0, SEEK_END); long len = ftell(fp); fseek(fp, 0, SEEK_SET);
    void *buf = aligned_alloc(16, (len + 15) / 16 * 16);
    if (fread(buf, 1, len, fp) != (size_t)len) return 2;
    fclose(fp);
    pol_net_t net;
    const char *err = pol_parse(buf, len, &net);
    if (err) { fprintf(stderr, "parse: %s\n", err); return 1; }
    float *work = malloc(sizeof(float) * 2 * net.max_width);
    printf("selftest max err %.3g over %u fixtures\n", pol_selftest(&net, work), net.h->n_fixtures);

    FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "w");
    const int n_steps = (int)rd(in);
    double bias[3], off[POL_N];
    for (int i = 0; i < 3; ++i) bias[i] = rd(in);
    for (int i = 0; i < POL_N; ++i) off[i] = rd(in);
    pol_velfilt_t vf; pol_vel_reset(&vf);
    pol_decoder_t dec; pol_decoder_reset(&dec);
    double pos[POL_N], acc[3], gyr[3];
    for (int t = 0; t < 1 + 4 * n_steps; ++t) {
        for (int i = 0; i < POL_N; ++i) pos[i] = pol_rel_from_dd(rd(in), net.h->signs[i], off[i]);
        for (int i = 0; i < 3; ++i) acc[i] = rd(in);
        for (int i = 0; i < 3; ++i) gyr[i] = rd(in);
        pol_vel_push(&vf, pos);
        if (t == 0 || t % 4 != 0) continue;
        double cmd[3]; for (int i = 0; i < 3; ++i) cmd[i] = rd(in);
        double gb[3], grav[3], vel[POL_N];
        pol_gyro_body(net.h, gyr, bias, gb);
        pol_gravity_body(net.h, acc, grav);
        pol_vel_get(&vf, vel);
        float obs[POL_OBS], raw[POL_ACT];
        pol_observe(gb, grav, cmd, pos, vel, dec.prev_action, obs);
        pol_forward(&net, obs, raw, work);
        int dd[POL_ACT];
        pol_decode(&dec, net.h, off, raw, dd);
        for (int i = 0; i < POL_OBS; ++i) fprintf(out, "%.9g ", obs[i]);
        for (int i = 0; i < POL_ACT; ++i) fprintf(out, "%.9g ", raw[i]);
        for (int i = 0; i < POL_ACT; ++i) fprintf(out, "%d ", dd[i]);
        fprintf(out, "\n");
    }
    fclose(out);
    return 0;
}
