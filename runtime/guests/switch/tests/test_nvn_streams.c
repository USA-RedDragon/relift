/**
 * @file test_nvn_streams.c
 * @brief The vertex streams a draw binds: every stream its pipeline reads, with gaps between them (no host device
 * needed: only the pipeline key is read).
 */
#include "nvn/nvn_pipe.h"
#include "tc.h"

TEST(nvn_a_draw_binds_vertex_streams_with_a_gap_between_them)
{
    nvn_pipe_key k = { 0 };
    k.bind[k.nbind++] = (VkVertexInputBindingDescription){ 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
    k.bind[k.nbind++] = (VkVertexInputBindingDescription){ 2, 8, VK_VERTEX_INPUT_RATE_VERTEX };
    k.bind[k.nbind++] = (VkVertexInputBindingDescription){ NVN_VERTEX_BUFFERS, 4, VK_VERTEX_INPUT_RATE_VERTEX };
    uint32_t s[NVN_VERTEX_BUFFERS];
    CHECK_EQ(nvn_draw_vertex_streams(&k, s), 2);
    CHECK_EQ(s[0], 0);
    CHECK_EQ(s[1], 2);
}
