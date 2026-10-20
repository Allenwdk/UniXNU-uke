/* SPDX-License-Identifier: MIT */
/* Host compiler fixture only. No rendering, submission or GPU access. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compiler/glsl_types.h"
#include "nir/nir_builder.h"
#include "freedreno_dev_info.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"

static FILE *output(const char *prefix, const char *suffix, const char *mode)
{
    char path[4096];
    int count = snprintf(path, sizeof(path), "%s.%s", prefix, suffix);
    if (count < 0 || (size_t)count >= sizeof(path)) return NULL;
    return fopen(path, mode);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const struct fd_dev_id id = {.chip_id = 0x43050a01}; /* pinned sheng DTS/catalog */
    const struct fd_dev_id unknown = {.chip_id = 0xdeadbeef};
    if (fd_dev_info_raw(&unknown)) return 3;
    const struct fd_dev_info *info = fd_dev_info_raw(&id);
    if (!info || fd_dev_gen(&id) != 7 || strcmp(fd_dev_name(&id), "FD740")) return 4;
    const struct ir3_compiler_options options = {
        .disable_cache = true,
        .bindless_fb_read_descriptor = -1,
        .bindless_fb_read_slot = -1,
    };
    struct ir3_compiler *compiler = ir3_compiler_create(NULL, &id, info, &options);
    if (!compiler || !(ir3_shader_debug & IR3_DBG_ASM_ROUNDTRIP)) return 5;
    glsl_type_singleton_init_or_ref();
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
        ir3_get_compiler_options(compiler), "offline-sheng-vertex-id");
    nir_def *x = nir_fadd(&b, nir_u2f32(&b, nir_load_vertex_id(&b)), nir_imm_float(&b, 0.5f));
    nir_def *position = nir_vec4(&b, x, nir_imm_float(&b, 0), nir_imm_float(&b, 0), nir_imm_float(&b, 1));
    nir_store_output(&b, position, nir_imm_int(&b, 0), .base = 0, .write_mask = 15,
                     .src_type = nir_type_float32,
                     .io_semantics = {.location = VARYING_SLOT_POS});
    b.shader->num_outputs = 1;
    nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
    nir_validate_shader(b.shader, "synthetic fixture before real IR3 compilation");
    FILE *stream = output(argv[1], "nir", "w");
    if (!stream) return 6;
    nir_print_shader(b.shader, stream);fclose(stream);
    const struct ir3_shader_options shader_options = {
        .api_wavesize = IR3_SINGLE_OR_DOUBLE,
        .real_wavesize = IR3_SINGLE_OR_DOUBLE,
    };
    struct ir3_shader *shader = ir3_shader_from_nir(compiler, b.shader, &shader_options);
    const struct ir3_shader_key key = {0};
    struct ir3_shader_variant *variant = ir3_shader_get_variant(shader, &key, false, true, NULL, NULL);
    if (!variant || !variant->bin || !variant->info.sizedwords || variant->info.size < variant->info.sizedwords * 4)
        return 7;
    stream = output(argv[1], "bin", "wb");
    if (!stream) return 8;
    size_t bytes = variant->info.size;
    if (fwrite(variant->bin, 1, bytes, stream) != bytes || fclose(stream)) return 9;
    stream = output(argv[1], "asm", "w");
    if (!stream) return 10;
    if (!variant->disasm_info.disasm || fputs(variant->disasm_info.disasm, stream) < 0 || fclose(stream)) return 11;
    printf("{\"chip_id\":\"0x43050a01\",\"name\":\"FD740\",\"generation\":7,\"stage\":\"vertex\","
           "\"binary_bytes\":%zu,\"code_dwords\":%u,\"unknown_chip_refused\":true,"
           "\"synthetic_nir\":true,\"asm_roundtrip\":true,\"gpu_executed\":false}\n", bytes, variant->info.sizedwords);
    ir3_shader_destroy(shader);
    glsl_type_singleton_decref();
    ir3_compiler_destroy(compiler);
    return 0;
}
