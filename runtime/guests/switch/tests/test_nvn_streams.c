/**
 * @file test_nvn_streams.c
 * @brief The vertex input a draw sets up: every stream its pipeline reads, with gaps between them, and the attribute
 * that pads an odd attribute count (no host device
 * needed: only the pipeline key is read).
 */
#include "nvn/nvn_pipe.h"
#include "tc.h"

TEST(nvn_the_attribute_padding_an_odd_count_is_the_constant_default)
{
    uint32_t state = 0, constant = 0;
    nvn_vertex_attribs_bound(&state, &constant, 5);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 4), NVN_ATTRIB_BOUND);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 5), NVN_ATTRIB_CONSTANT);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 6), NVN_ATTRIB_UNKNOWN);
    state = constant = 0;
    nvn_vertex_attribs_bound(&state, &constant, 4);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 4), NVN_ATTRIB_UNKNOWN);
}

TEST(nvn_attributes_past_a_bind_keep_what_an_earlier_bind_wrote)
{
    uint32_t state = 0, constant = 0;
    nvn_vertex_attribs_bound(&state, &constant, 10);
    nvn_vertex_attribs_bound(&state, &constant, 5);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 4), NVN_ATTRIB_BOUND);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 5), NVN_ATTRIB_CONSTANT);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 6), NVN_ATTRIB_BOUND);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 9), NVN_ATTRIB_BOUND);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 10), NVN_ATTRIB_UNKNOWN);
    nvn_vertex_attribs_bound(&state, &constant, 6);
    CHECK_EQ(nvn_vertex_attrib_source(state, constant, 5), NVN_ATTRIB_BOUND);
}

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
