/*
 * Copyright (c) 2025, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

// This file is generated. Do not edit.
#ifndef AV1_RTCD_H_
#define AV1_RTCD_H_

#ifdef RTCD_C
#define RTCD_EXTERN
#else
#define RTCD_EXTERN extern
#endif

/*
 * AV1
 */

#include "aom/aom_integer.h"
#include "aom_dsp/odintrin.h"
#include "aom_dsp/txfm_common.h"
#include "av1/common/av1_txfm.h"
#include "av1/common/common.h"
#include "av1/common/convolve.h"
#include "av1/common/enums.h"
#include "av1/common/filter.h"
#include "av1/common/quant_common.h"
#include "av1/common/restoration.h"

struct macroblockd;

/* Encoder forward decls */
struct macroblock;
struct txfm_param;
struct aom_variance_vtable;
struct search_site_config;
struct yv12_buffer_config;
struct NN_CONFIG;
typedef struct NN_CONFIG NN_CONFIG;

enum { NONE, RELU, SOFTSIGN, SIGMOID } UENUM1BYTE(ACTIVATION);
#if CONFIG_NN_V2
enum { SOFTMAX_CROSS_ENTROPY } UENUM1BYTE(LOSS);
struct NN_CONFIG_V2;
typedef struct NN_CONFIG_V2 NN_CONFIG_V2;
struct FC_LAYER;
typedef struct FC_LAYER FC_LAYER;
#endif  // CONFIG_NN_V2

struct CNN_CONFIG;
typedef struct CNN_CONFIG CNN_CONFIG;
struct CNN_LAYER_CONFIG;
typedef struct CNN_LAYER_CONFIG CNN_LAYER_CONFIG;
struct CNN_THREAD_DATA;
typedef struct CNN_THREAD_DATA CNN_THREAD_DATA;
struct CNN_BRANCH_CONFIG;
typedef struct CNN_BRANCH_CONFIG CNN_BRANCH_CONFIG;
struct CNN_MULTI_OUT;
typedef struct CNN_MULTI_OUT CNN_MULTI_OUT;

/* Function pointers return by CfL functions */
typedef void (*cfl_subsample_lbd_fn)(const uint8_t *input, int input_stride,
                                     uint16_t *output_q3);

#if CONFIG_AV1_HIGHBITDEPTH
typedef void (*cfl_subsample_hbd_fn)(const uint16_t *input, int input_stride,
                                     uint16_t *output_q3);

typedef void (*cfl_predict_hbd_fn)(const int16_t *src, uint16_t *dst,
                                   int dst_stride, int alpha_q3, int bd);
#endif

typedef void (*cfl_subtract_average_fn)(const uint16_t *src, int16_t *dst);

typedef void (*cfl_predict_lbd_fn)(const int16_t *src, uint8_t *dst,
                                   int dst_stride, int alpha_q3);


#ifdef __cplusplus
extern "C" {
#endif

void aom_comp_avg_upsampled_pred_c(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                                   const MV *const mv, uint8_t *comp_pred, const uint8_t *pred, int width,
                                                   int height, int subpel_x_q3, int subpel_y_q3, const uint8_t *ref,
                                                   int ref_stride, int subpel_search);
void aom_comp_avg_upsampled_pred_neon(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                                   const MV *const mv, uint8_t *comp_pred, const uint8_t *pred, int width,
                                                   int height, int subpel_x_q3, int subpel_y_q3, const uint8_t *ref,
                                                   int ref_stride, int subpel_search);
#define aom_comp_avg_upsampled_pred aom_comp_avg_upsampled_pred_neon

void aom_highbd_comp_avg_upsampled_pred_c(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                                            const MV *const mv, uint8_t *comp_pred8, const uint8_t *pred8, int width,
                                                            int height, int subpel_x_q3, int subpel_y_q3, const uint8_t *ref8, int ref_stride, int bd, int subpel_search);
void aom_highbd_comp_avg_upsampled_pred_neon(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                                            const MV *const mv, uint8_t *comp_pred8, const uint8_t *pred8, int width,
                                                            int height, int subpel_x_q3, int subpel_y_q3, const uint8_t *ref8, int ref_stride, int bd, int subpel_search);
#define aom_highbd_comp_avg_upsampled_pred aom_highbd_comp_avg_upsampled_pred_neon

void aom_highbd_upsampled_pred_c(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                                   const MV *const mv, uint8_t *comp_pred8, int width, int height, int subpel_x_q3,
                                                   int subpel_y_q3, const uint8_t *ref8, int ref_stride, int bd, int subpel_search);
void aom_highbd_upsampled_pred_neon(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                                   const MV *const mv, uint8_t *comp_pred8, int width, int height, int subpel_x_q3,
                                                   int subpel_y_q3, const uint8_t *ref8, int ref_stride, int bd, int subpel_search);
#define aom_highbd_upsampled_pred aom_highbd_upsampled_pred_neon

void aom_quantize_b_helper_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan, const qm_val_t *qm_ptr, const qm_val_t *iqm_ptr, const int log_scale);
void aom_quantize_b_helper_neon(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan, const qm_val_t *qm_ptr, const qm_val_t *iqm_ptr, const int log_scale);
#define aom_quantize_b_helper aom_quantize_b_helper_neon

void aom_upsampled_pred_c(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                          const MV *const mv, uint8_t *comp_pred, int width, int height, int subpel_x_q3,
                                          int subpel_y_q3, const uint8_t *ref, int ref_stride, int subpel_search);
void aom_upsampled_pred_neon(MACROBLOCKD *xd, const struct AV1Common *const cm, int mi_row, int mi_col,
                                          const MV *const mv, uint8_t *comp_pred, int width, int height, int subpel_x_q3,
                                          int subpel_y_q3, const uint8_t *ref, int ref_stride, int subpel_search);
#define aom_upsampled_pred aom_upsampled_pred_neon

int av1_apply_selfguided_restoration_c(const uint8_t *dat, int width, int height, int stride, int eps, const int *xqd, uint8_t *dst, int dst_stride, int32_t *tmpbuf, int bit_depth, int highbd);
int av1_apply_selfguided_restoration_neon(const uint8_t *dat, int width, int height, int stride, int eps, const int *xqd, uint8_t *dst, int dst_stride, int32_t *tmpbuf, int bit_depth, int highbd);
#define av1_apply_selfguided_restoration av1_apply_selfguided_restoration_neon

void av1_apply_temporal_filter_c(const struct yv12_buffer_config *frame_to_filter, const struct macroblockd *mbd, const BLOCK_SIZE block_size, const int mb_row, const int mb_col, const int num_planes, const double *noise_levels, const MV *subblock_mvs, const int *subblock_mses, const int q_factor, const int filter_strength, int tf_wgt_calc_lvl, const uint8_t *pred, uint32_t *accum, uint16_t *count);
void av1_apply_temporal_filter_neon(const struct yv12_buffer_config *frame_to_filter, const struct macroblockd *mbd, const BLOCK_SIZE block_size, const int mb_row, const int mb_col, const int num_planes, const double *noise_levels, const MV *subblock_mvs, const int *subblock_mses, const int q_factor, const int filter_strength, int tf_wgt_calc_lvl, const uint8_t *pred, uint32_t *accum, uint16_t *count);
void av1_apply_temporal_filter_neon_dotprod(const struct yv12_buffer_config *frame_to_filter, const struct macroblockd *mbd, const BLOCK_SIZE block_size, const int mb_row, const int mb_col, const int num_planes, const double *noise_levels, const MV *subblock_mvs, const int *subblock_mses, const int q_factor, const int filter_strength, int tf_wgt_calc_lvl, const uint8_t *pred, uint32_t *accum, uint16_t *count);
RTCD_EXTERN void (*av1_apply_temporal_filter)(const struct yv12_buffer_config *frame_to_filter, const struct macroblockd *mbd, const BLOCK_SIZE block_size, const int mb_row, const int mb_col, const int num_planes, const double *noise_levels, const MV *subblock_mvs, const int *subblock_mses, const int q_factor, const int filter_strength, int tf_wgt_calc_lvl, const uint8_t *pred, uint32_t *accum, uint16_t *count);

int64_t av1_block_error_c(const tran_low_t *coeff, const tran_low_t *dqcoeff, intptr_t block_size, int64_t *ssz);
int64_t av1_block_error_neon(const tran_low_t *coeff, const tran_low_t *dqcoeff, intptr_t block_size, int64_t *ssz);
#define av1_block_error av1_block_error_neon

int64_t av1_block_error_lp_c(const int16_t *coeff, const int16_t *dqcoeff, intptr_t block_size);
int64_t av1_block_error_lp_neon(const int16_t *coeff, const int16_t *dqcoeff, intptr_t block_size);
#define av1_block_error_lp av1_block_error_lp_neon

void av1_build_compound_diffwtd_mask_c(uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const uint8_t *src0, int src0_stride, const uint8_t *src1, int src1_stride, int h, int w);
void av1_build_compound_diffwtd_mask_neon(uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const uint8_t *src0, int src0_stride, const uint8_t *src1, int src1_stride, int h, int w);
#define av1_build_compound_diffwtd_mask av1_build_compound_diffwtd_mask_neon

void av1_build_compound_diffwtd_mask_d16_c(uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const CONV_BUF_TYPE *src0, int src0_stride, const CONV_BUF_TYPE *src1, int src1_stride, int h, int w, ConvolveParams *conv_params, int bd);
void av1_build_compound_diffwtd_mask_d16_neon(uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const CONV_BUF_TYPE *src0, int src0_stride, const CONV_BUF_TYPE *src1, int src1_stride, int h, int w, ConvolveParams *conv_params, int bd);
#define av1_build_compound_diffwtd_mask_d16 av1_build_compound_diffwtd_mask_d16_neon

void av1_build_compound_diffwtd_mask_highbd_c(uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const uint8_t *src0, int src0_stride, const uint8_t *src1, int src1_stride, int h, int w, int bd);
void av1_build_compound_diffwtd_mask_highbd_neon(uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const uint8_t *src0, int src0_stride, const uint8_t *src1, int src1_stride, int h, int w, int bd);
#define av1_build_compound_diffwtd_mask_highbd av1_build_compound_diffwtd_mask_highbd_neon

void av1_calc_indices_dim1_c(const int16_t *data, const int16_t *centroids, uint8_t *indices, int64_t *total_dist, int n, int k);
void av1_calc_indices_dim1_neon(const int16_t *data, const int16_t *centroids, uint8_t *indices, int64_t *total_dist, int n, int k);
#define av1_calc_indices_dim1 av1_calc_indices_dim1_neon

void av1_calc_indices_dim2_c(const int16_t *data, const int16_t *centroids, uint8_t *indices, int64_t *total_dist, int n, int k);
void av1_calc_indices_dim2_neon(const int16_t *data, const int16_t *centroids, uint8_t *indices, int64_t *total_dist, int n, int k);
#define av1_calc_indices_dim2 av1_calc_indices_dim2_neon

void av1_calc_proj_params_c(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int64_t H[2][2], int64_t C[2], const sgr_params_type *params);
void av1_calc_proj_params_neon(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int64_t H[2][2], int64_t C[2], const sgr_params_type *params);
#define av1_calc_proj_params av1_calc_proj_params_neon

void av1_calc_proj_params_high_bd_c(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int64_t H[2][2], int64_t C[2], const sgr_params_type *params);
void av1_calc_proj_params_high_bd_neon(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int64_t H[2][2], int64_t C[2], const sgr_params_type *params);
#define av1_calc_proj_params_high_bd av1_calc_proj_params_high_bd_neon

void av1_cnn_activate_c(float **input, int channels, int width, int height, int stride, ACTIVATION layer_activation);
#define av1_cnn_activate av1_cnn_activate_c

void av1_cnn_add_c(float **input, int channels, int width, int height, int stride, const float **add);
#define av1_cnn_add av1_cnn_add_c

void av1_cnn_batchnorm_c(float **image, int channels, int width, int height, int stride, const float *gamma, const float *beta, const float *mean, const float *std);
#define av1_cnn_batchnorm av1_cnn_batchnorm_c

void av1_cnn_convolve_no_maxpool_padding_valid_c(const float **input, int in_width, int in_height, int in_stride, const CNN_LAYER_CONFIG *layer_config, float **output, int out_stride, int start_idx, int cstep, int channel_step);
void av1_cnn_convolve_no_maxpool_padding_valid_neon(const float **input, int in_width, int in_height, int in_stride, const CNN_LAYER_CONFIG *layer_config, float **output, int out_stride, int start_idx, int cstep, int channel_step);
#define av1_cnn_convolve_no_maxpool_padding_valid av1_cnn_convolve_no_maxpool_padding_valid_neon

void av1_cnn_deconvolve_c(const float **input, int in_width, int in_height, int in_stride, const CNN_LAYER_CONFIG *layer_config, float **output, int out_stride);
#define av1_cnn_deconvolve av1_cnn_deconvolve_c

bool av1_cnn_predict_c(const float **input, int in_width, int in_height, int in_stride, const CNN_CONFIG *cnn_config, const CNN_THREAD_DATA *thread_data, CNN_MULTI_OUT *output_struct);
#define av1_cnn_predict av1_cnn_predict_c

void av1_compute_stats_c(int wiener_win, const uint8_t *dgd8, const uint8_t *src8, int16_t *dgd_avg, int16_t *src_avg, int h_start, int h_end, int v_start, int v_end, int dgd_stride, int src_stride, int64_t *M, int64_t *H, int use_downsampled_wiener_stats);
void av1_compute_stats_neon(int wiener_win, const uint8_t *dgd8, const uint8_t *src8, int16_t *dgd_avg, int16_t *src_avg, int h_start, int h_end, int v_start, int v_end, int dgd_stride, int src_stride, int64_t *M, int64_t *H, int use_downsampled_wiener_stats);
#define av1_compute_stats av1_compute_stats_neon

void av1_compute_stats_highbd_c(int wiener_win, const uint8_t *dgd8, const uint8_t *src8, int16_t *dgd_avg, int16_t *src_avg, int h_start, int h_end, int v_start, int v_end, int dgd_stride, int src_stride, int64_t *M, int64_t *H, aom_bit_depth_t bit_depth);
void av1_compute_stats_highbd_neon(int wiener_win, const uint8_t *dgd8, const uint8_t *src8, int16_t *dgd_avg, int16_t *src_avg, int h_start, int h_end, int v_start, int v_end, int dgd_stride, int src_stride, int64_t *M, int64_t *H, aom_bit_depth_t bit_depth);
#define av1_compute_stats_highbd av1_compute_stats_highbd_neon

void av1_convolve_2d_scale_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int x_step_qn, const int subpel_y_qn, const int y_step_qn, ConvolveParams *conv_params);
void av1_convolve_2d_scale_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int x_step_qn, const int subpel_y_qn, const int y_step_qn, ConvolveParams *conv_params);
void av1_convolve_2d_scale_neon_dotprod(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int x_step_qn, const int subpel_y_qn, const int y_step_qn, ConvolveParams *conv_params);
void av1_convolve_2d_scale_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int x_step_qn, const int subpel_y_qn, const int y_step_qn, ConvolveParams *conv_params);
RTCD_EXTERN void (*av1_convolve_2d_scale)(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int x_step_qn, const int subpel_y_qn, const int y_step_qn, ConvolveParams *conv_params);

void av1_convolve_2d_sr_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_convolve_2d_sr_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_convolve_2d_sr_neon_dotprod(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_convolve_2d_sr_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
RTCD_EXTERN void (*av1_convolve_2d_sr)(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);

void av1_convolve_2d_sr_intrabc_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_convolve_2d_sr_intrabc_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
#define av1_convolve_2d_sr_intrabc av1_convolve_2d_sr_intrabc_neon

void av1_convolve_horiz_rs_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const int16_t *x_filters, int x0_qn, int x_step_qn);
void av1_convolve_horiz_rs_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const int16_t *x_filters, int x0_qn, int x_step_qn);
#define av1_convolve_horiz_rs av1_convolve_horiz_rs_neon

void av1_convolve_x_sr_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
void av1_convolve_x_sr_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
void av1_convolve_x_sr_neon_dotprod(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
void av1_convolve_x_sr_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
RTCD_EXTERN void (*av1_convolve_x_sr)(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);

void av1_convolve_x_sr_intrabc_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
void av1_convolve_x_sr_intrabc_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
#define av1_convolve_x_sr_intrabc av1_convolve_x_sr_intrabc_neon

void av1_convolve_y_sr_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn);
void av1_convolve_y_sr_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn);
void av1_convolve_y_sr_neon_dotprod(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn);
void av1_convolve_y_sr_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn);
RTCD_EXTERN void (*av1_convolve_y_sr)(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn);

void av1_convolve_y_sr_intrabc_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn);
void av1_convolve_y_sr_intrabc_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn);
#define av1_convolve_y_sr_intrabc av1_convolve_y_sr_intrabc_neon

void av1_dist_wtd_convolve_2d_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_2d_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_2d_neon_dotprod(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_2d_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);
RTCD_EXTERN void (*av1_dist_wtd_convolve_2d)(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params);

void av1_dist_wtd_convolve_2d_copy_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_2d_copy_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, ConvolveParams *conv_params);
#define av1_dist_wtd_convolve_2d_copy av1_dist_wtd_convolve_2d_copy_neon

void av1_dist_wtd_convolve_x_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_x_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_x_neon_dotprod(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_x_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);
RTCD_EXTERN void (*av1_dist_wtd_convolve_x)(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params);

void av1_dist_wtd_convolve_y_c(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, ConvolveParams *conv_params);
void av1_dist_wtd_convolve_y_neon(const uint8_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, ConvolveParams *conv_params);
#define av1_dist_wtd_convolve_y av1_dist_wtd_convolve_y_neon

void av1_dr_prediction_z1_c(uint8_t *dst, ptrdiff_t stride, int bw, int bh, const uint8_t *above, const uint8_t *left, int upsample_above, int dx, int dy);
void av1_dr_prediction_z1_neon(uint8_t *dst, ptrdiff_t stride, int bw, int bh, const uint8_t *above, const uint8_t *left, int upsample_above, int dx, int dy);
#define av1_dr_prediction_z1 av1_dr_prediction_z1_neon

void av1_dr_prediction_z2_c(uint8_t *dst, ptrdiff_t stride, int bw, int bh, const uint8_t *above, const uint8_t *left, int upsample_above, int upsample_left, int dx, int dy);
void av1_dr_prediction_z2_neon(uint8_t *dst, ptrdiff_t stride, int bw, int bh, const uint8_t *above, const uint8_t *left, int upsample_above, int upsample_left, int dx, int dy);
#define av1_dr_prediction_z2 av1_dr_prediction_z2_neon

void av1_dr_prediction_z3_c(uint8_t *dst, ptrdiff_t stride, int bw, int bh, const uint8_t *above, const uint8_t *left, int upsample_left, int dx, int dy);
void av1_dr_prediction_z3_neon(uint8_t *dst, ptrdiff_t stride, int bw, int bh, const uint8_t *above, const uint8_t *left, int upsample_left, int dx, int dy);
#define av1_dr_prediction_z3 av1_dr_prediction_z3_neon

double av1_estimate_noise_from_single_plane_c(const uint8_t *src, int height, int width, int stride, int edge_thresh);
double av1_estimate_noise_from_single_plane_neon(const uint8_t *src, int height, int width, int stride, int edge_thresh);
#define av1_estimate_noise_from_single_plane av1_estimate_noise_from_single_plane_neon

void av1_fdwt8x8_uint8_input_c(const uint8_t *input, tran_low_t *output, int stride, int hbd);
#define av1_fdwt8x8_uint8_input av1_fdwt8x8_uint8_input_c

void av1_filter_intra_edge_c(uint8_t *p, int sz, int strength);
void av1_filter_intra_edge_neon(uint8_t *p, int sz, int strength);
#define av1_filter_intra_edge av1_filter_intra_edge_neon

void av1_filter_intra_predictor_c(uint8_t *dst, ptrdiff_t stride, TX_SIZE tx_size, const uint8_t *above, const uint8_t *left, int mode);
void av1_filter_intra_predictor_neon(uint8_t *dst, ptrdiff_t stride, TX_SIZE tx_size, const uint8_t *above, const uint8_t *left, int mode);
#define av1_filter_intra_predictor av1_filter_intra_predictor_neon

void av1_fwd_txfm2d_16x16_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_16x16_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_16x16 av1_fwd_txfm2d_16x16_neon

void av1_fwd_txfm2d_16x32_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_16x32_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_16x32 av1_fwd_txfm2d_16x32_neon

void av1_fwd_txfm2d_16x4_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_16x4_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_16x4 av1_fwd_txfm2d_16x4_neon

void av1_fwd_txfm2d_16x64_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_16x64_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_16x64 av1_fwd_txfm2d_16x64_neon

void av1_fwd_txfm2d_16x8_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_16x8_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_16x8 av1_fwd_txfm2d_16x8_neon

void av1_fwd_txfm2d_32x16_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_32x16_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_32x16 av1_fwd_txfm2d_32x16_neon

void av1_fwd_txfm2d_32x32_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_32x32_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_32x32 av1_fwd_txfm2d_32x32_neon

void av1_fwd_txfm2d_32x64_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_32x64_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_32x64 av1_fwd_txfm2d_32x64_neon

void av1_fwd_txfm2d_32x8_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_32x8_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_32x8 av1_fwd_txfm2d_32x8_neon

void av1_fwd_txfm2d_4x16_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_4x16_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_4x16 av1_fwd_txfm2d_4x16_neon

void av1_fwd_txfm2d_4x4_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_4x4_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_4x4 av1_fwd_txfm2d_4x4_neon

void av1_fwd_txfm2d_4x8_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_4x8_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_4x8 av1_fwd_txfm2d_4x8_neon

void av1_fwd_txfm2d_64x16_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_64x16_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_64x16 av1_fwd_txfm2d_64x16_neon

void av1_fwd_txfm2d_64x32_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_64x32_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_64x32 av1_fwd_txfm2d_64x32_neon

void av1_fwd_txfm2d_64x64_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_64x64_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_64x64 av1_fwd_txfm2d_64x64_neon

void av1_fwd_txfm2d_8x16_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_8x16_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_8x16 av1_fwd_txfm2d_8x16_neon

void av1_fwd_txfm2d_8x32_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_8x32_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_8x32 av1_fwd_txfm2d_8x32_neon

void av1_fwd_txfm2d_8x4_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_8x4_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_8x4 av1_fwd_txfm2d_8x4_neon

void av1_fwd_txfm2d_8x8_c(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_fwd_txfm2d_8x8_neon(const int16_t *input, int32_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_fwd_txfm2d_8x8 av1_fwd_txfm2d_8x8_neon

void av1_fwht4x4_c(const int16_t *input, tran_low_t *output, int stride);
void av1_fwht4x4_neon(const int16_t *input, tran_low_t *output, int stride);
#define av1_fwht4x4 av1_fwht4x4_neon

uint32_t av1_get_crc32c_value_c(void *crc_calculator, const uint8_t *p, size_t length);
uint32_t av1_get_crc32c_value_arm_crc32(void *crc_calculator, const uint8_t *p, size_t length);
RTCD_EXTERN uint32_t (*av1_get_crc32c_value)(void *crc_calculator, const uint8_t *p, size_t length);

void av1_get_horver_correlation_full_c(const int16_t *diff, int stride, int w, int h, float *hcorr, float *vcorr);
void av1_get_horver_correlation_full_neon(const int16_t *diff, int stride, int w, int h, float *hcorr, float *vcorr);
#define av1_get_horver_correlation_full av1_get_horver_correlation_full_neon

void av1_get_nz_map_contexts_c(const uint8_t *const levels, const int16_t *const scan, const uint16_t eob, const TX_SIZE tx_size, const TX_CLASS tx_class, int8_t *const coeff_contexts);
void av1_get_nz_map_contexts_neon(const uint8_t *const levels, const int16_t *const scan, const uint16_t eob, const TX_SIZE tx_size, const TX_CLASS tx_class, int8_t *const coeff_contexts);
#define av1_get_nz_map_contexts av1_get_nz_map_contexts_neon

void av1_highbd_apply_temporal_filter_c(const struct yv12_buffer_config *frame_to_filter, const struct macroblockd *mbd, const BLOCK_SIZE block_size, const int mb_row, const int mb_col, const int num_planes, const double *noise_levels, const MV *subblock_mvs, const int *subblock_mses, const int q_factor, const int filter_strength, int tf_wgt_calc_lvl, const uint8_t *pred, uint32_t *accum, uint16_t *count);
void av1_highbd_apply_temporal_filter_neon(const struct yv12_buffer_config *frame_to_filter, const struct macroblockd *mbd, const BLOCK_SIZE block_size, const int mb_row, const int mb_col, const int num_planes, const double *noise_levels, const MV *subblock_mvs, const int *subblock_mses, const int q_factor, const int filter_strength, int tf_wgt_calc_lvl, const uint8_t *pred, uint32_t *accum, uint16_t *count);
#define av1_highbd_apply_temporal_filter av1_highbd_apply_temporal_filter_neon

int64_t av1_highbd_block_error_c(const tran_low_t *coeff, const tran_low_t *dqcoeff, intptr_t block_size, int64_t *ssz, int bd);
int64_t av1_highbd_block_error_neon(const tran_low_t *coeff, const tran_low_t *dqcoeff, intptr_t block_size, int64_t *ssz, int bd);
#define av1_highbd_block_error av1_highbd_block_error_neon

void av1_highbd_convolve8_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, int bps);
#define av1_highbd_convolve8 av1_highbd_convolve8_c

void av1_highbd_convolve8_horiz_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, int bps);
#define av1_highbd_convolve8_horiz av1_highbd_convolve8_horiz_c

void av1_highbd_convolve8_vert_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, int bps);
#define av1_highbd_convolve8_vert av1_highbd_convolve8_vert_c

void av1_highbd_convolve_2d_scale_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int x_step_qn, const int subpel_y_qn, const int y_step_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_convolve_2d_scale_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int x_step_qn, const int subpel_y_qn, const int y_step_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_convolve_2d_scale av1_highbd_convolve_2d_scale_neon

void av1_highbd_convolve_2d_sr_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_convolve_2d_sr_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_convolve_2d_sr av1_highbd_convolve_2d_sr_neon

void av1_highbd_convolve_2d_sr_intrabc_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_convolve_2d_sr_intrabc_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_convolve_2d_sr_intrabc av1_highbd_convolve_2d_sr_intrabc_neon

void av1_highbd_convolve_avg_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, int bps);
#define av1_highbd_convolve_avg av1_highbd_convolve_avg_c

void av1_highbd_convolve_copy_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, int bps);
#define av1_highbd_convolve_copy av1_highbd_convolve_copy_c

void av1_highbd_convolve_horiz_rs_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const int16_t *x_filters, int x0_qn, int x_step_qn, int bd);
void av1_highbd_convolve_horiz_rs_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const int16_t *x_filters, int x0_qn, int x_step_qn, int bd);
#define av1_highbd_convolve_horiz_rs av1_highbd_convolve_horiz_rs_neon

void av1_highbd_convolve_x_sr_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_convolve_x_sr_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_convolve_x_sr av1_highbd_convolve_x_sr_neon

void av1_highbd_convolve_x_sr_intrabc_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_convolve_x_sr_intrabc_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_convolve_x_sr_intrabc av1_highbd_convolve_x_sr_intrabc_neon

void av1_highbd_convolve_y_sr_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, int bd);
void av1_highbd_convolve_y_sr_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, int bd);
#define av1_highbd_convolve_y_sr av1_highbd_convolve_y_sr_neon

void av1_highbd_convolve_y_sr_intrabc_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, int bd);
void av1_highbd_convolve_y_sr_intrabc_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, int bd);
#define av1_highbd_convolve_y_sr_intrabc av1_highbd_convolve_y_sr_intrabc_neon

void av1_highbd_dist_wtd_convolve_2d_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_dist_wtd_convolve_2d_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const InterpFilterParams *filter_params_y, const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_dist_wtd_convolve_2d av1_highbd_dist_wtd_convolve_2d_neon

void av1_highbd_dist_wtd_convolve_2d_copy_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, ConvolveParams *conv_params, int bd);
void av1_highbd_dist_wtd_convolve_2d_copy_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, ConvolveParams *conv_params, int bd);
#define av1_highbd_dist_wtd_convolve_2d_copy av1_highbd_dist_wtd_convolve_2d_copy_neon

void av1_highbd_dist_wtd_convolve_x_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_dist_wtd_convolve_x_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_x, const int subpel_x_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_dist_wtd_convolve_x av1_highbd_dist_wtd_convolve_x_neon

void av1_highbd_dist_wtd_convolve_y_c(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
void av1_highbd_dist_wtd_convolve_y_neon(const uint16_t *src, int src_stride, uint16_t *dst, int dst_stride, int w, int h, const InterpFilterParams *filter_params_y, const int subpel_y_qn, ConvolveParams *conv_params, int bd);
#define av1_highbd_dist_wtd_convolve_y av1_highbd_dist_wtd_convolve_y_neon

void av1_highbd_dr_prediction_z1_c(uint16_t *dst, ptrdiff_t stride, int bw, int bh, const uint16_t *above, const uint16_t *left, int upsample_above, int dx, int dy, int bd);
void av1_highbd_dr_prediction_z1_neon(uint16_t *dst, ptrdiff_t stride, int bw, int bh, const uint16_t *above, const uint16_t *left, int upsample_above, int dx, int dy, int bd);
#define av1_highbd_dr_prediction_z1 av1_highbd_dr_prediction_z1_neon

void av1_highbd_dr_prediction_z2_c(uint16_t *dst, ptrdiff_t stride, int bw, int bh, const uint16_t *above, const uint16_t *left, int upsample_above, int upsample_left, int dx, int dy, int bd);
void av1_highbd_dr_prediction_z2_neon(uint16_t *dst, ptrdiff_t stride, int bw, int bh, const uint16_t *above, const uint16_t *left, int upsample_above, int upsample_left, int dx, int dy, int bd);
#define av1_highbd_dr_prediction_z2 av1_highbd_dr_prediction_z2_neon

void av1_highbd_dr_prediction_z3_c(uint16_t *dst, ptrdiff_t stride, int bw, int bh, const uint16_t *above, const uint16_t *left, int upsample_left, int dx, int dy, int bd);
void av1_highbd_dr_prediction_z3_neon(uint16_t *dst, ptrdiff_t stride, int bw, int bh, const uint16_t *above, const uint16_t *left, int upsample_left, int dx, int dy, int bd);
#define av1_highbd_dr_prediction_z3 av1_highbd_dr_prediction_z3_neon

double av1_highbd_estimate_noise_from_single_plane_c(const uint16_t *src, int height, int width, int stride, int bit_depth, int edge_thresh);
double av1_highbd_estimate_noise_from_single_plane_neon(const uint16_t *src, int height, int width, int stride, int bit_depth, int edge_thresh);
#define av1_highbd_estimate_noise_from_single_plane av1_highbd_estimate_noise_from_single_plane_neon

void av1_highbd_filter_intra_edge_c(uint16_t *p, int sz, int strength);
void av1_highbd_filter_intra_edge_neon(uint16_t *p, int sz, int strength);
#define av1_highbd_filter_intra_edge av1_highbd_filter_intra_edge_neon

void av1_highbd_inv_txfm_add_c(const tran_low_t *input, uint8_t *dest, int stride, const TxfmParam *txfm_param);
void av1_highbd_inv_txfm_add_neon(const tran_low_t *input, uint8_t *dest, int stride, const TxfmParam *txfm_param);
#define av1_highbd_inv_txfm_add av1_highbd_inv_txfm_add_neon

void av1_highbd_iwht4x4_16_add_c(const tran_low_t *input, uint8_t *dest, int dest_stride, int bd);
#define av1_highbd_iwht4x4_16_add av1_highbd_iwht4x4_16_add_c

void av1_highbd_iwht4x4_1_add_c(const tran_low_t *input, uint8_t *dest, int dest_stride, int bd);
#define av1_highbd_iwht4x4_1_add av1_highbd_iwht4x4_1_add_c

int64_t av1_highbd_pixel_proj_error_c(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int xq[2], const sgr_params_type *params);
int64_t av1_highbd_pixel_proj_error_neon(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int xq[2], const sgr_params_type *params);
#define av1_highbd_pixel_proj_error av1_highbd_pixel_proj_error_neon

void av1_highbd_quantize_fp_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan, int log_scale);
void av1_highbd_quantize_fp_neon(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan, int log_scale);
#define av1_highbd_quantize_fp av1_highbd_quantize_fp_neon

void av1_highbd_upsample_intra_edge_c(uint16_t *p, int sz, int bd);
void av1_highbd_upsample_intra_edge_neon(uint16_t *p, int sz, int bd);
#define av1_highbd_upsample_intra_edge av1_highbd_upsample_intra_edge_neon

void av1_highbd_warp_affine_c(const int32_t *mat, const uint16_t *ref, int width, int height, int stride, uint16_t *pred, int p_col, int p_row, int p_width, int p_height, int p_stride, int subsampling_x, int subsampling_y, int bd, ConvolveParams *conv_params, int16_t alpha, int16_t beta, int16_t gamma, int16_t delta);
void av1_highbd_warp_affine_neon(const int32_t *mat, const uint16_t *ref, int width, int height, int stride, uint16_t *pred, int p_col, int p_row, int p_width, int p_height, int p_stride, int subsampling_x, int subsampling_y, int bd, ConvolveParams *conv_params, int16_t alpha, int16_t beta, int16_t gamma, int16_t delta);
#define av1_highbd_warp_affine av1_highbd_warp_affine_neon

void av1_highbd_wiener_convolve_add_src_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, const WienerConvolveParams *conv_params, int bd);
void av1_highbd_wiener_convolve_add_src_neon(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, const WienerConvolveParams *conv_params, int bd);
#define av1_highbd_wiener_convolve_add_src av1_highbd_wiener_convolve_add_src_neon

void av1_inv_txfm2d_add_16x16_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_16x16 av1_inv_txfm2d_add_16x16_c

void av1_inv_txfm2d_add_16x32_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_16x32_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_16x32 av1_inv_txfm2d_add_16x32_neon

void av1_inv_txfm2d_add_16x4_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_16x4_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_16x4 av1_inv_txfm2d_add_16x4_neon

void av1_inv_txfm2d_add_16x64_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_16x64_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_16x64 av1_inv_txfm2d_add_16x64_neon

void av1_inv_txfm2d_add_16x8_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_16x8_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_16x8 av1_inv_txfm2d_add_16x8_neon

void av1_inv_txfm2d_add_32x16_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_32x16_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_32x16 av1_inv_txfm2d_add_32x16_neon

void av1_inv_txfm2d_add_32x32_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_32x32_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_32x32 av1_inv_txfm2d_add_32x32_neon

void av1_inv_txfm2d_add_32x64_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_32x64_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_32x64 av1_inv_txfm2d_add_32x64_neon

void av1_inv_txfm2d_add_32x8_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_32x8_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_32x8 av1_inv_txfm2d_add_32x8_neon

void av1_inv_txfm2d_add_4x16_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_4x16_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_4x16 av1_inv_txfm2d_add_4x16_neon

void av1_inv_txfm2d_add_4x4_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_4x4_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_4x4 av1_inv_txfm2d_add_4x4_neon

void av1_inv_txfm2d_add_4x8_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_4x8_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_4x8 av1_inv_txfm2d_add_4x8_neon

void av1_inv_txfm2d_add_64x16_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_64x16_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_64x16 av1_inv_txfm2d_add_64x16_neon

void av1_inv_txfm2d_add_64x32_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_64x32_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_64x32 av1_inv_txfm2d_add_64x32_neon

void av1_inv_txfm2d_add_64x64_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_64x64_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_64x64 av1_inv_txfm2d_add_64x64_neon

void av1_inv_txfm2d_add_8x16_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_8x16_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_8x16 av1_inv_txfm2d_add_8x16_neon

void av1_inv_txfm2d_add_8x32_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_8x32_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_8x32 av1_inv_txfm2d_add_8x32_neon

void av1_inv_txfm2d_add_8x4_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_8x4_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_8x4 av1_inv_txfm2d_add_8x4_neon

void av1_inv_txfm2d_add_8x8_c(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
void av1_inv_txfm2d_add_8x8_neon(const int32_t *input, uint16_t *output, int stride, TX_TYPE tx_type, int bd);
#define av1_inv_txfm2d_add_8x8 av1_inv_txfm2d_add_8x8_neon

void av1_inv_txfm_add_c(const tran_low_t *dqcoeff, uint8_t *dst, int stride, const TxfmParam *txfm_param);
void av1_inv_txfm_add_neon(const tran_low_t *dqcoeff, uint8_t *dst, int stride, const TxfmParam *txfm_param);
#define av1_inv_txfm_add av1_inv_txfm_add_neon

void av1_lowbd_fwd_txfm_c(const int16_t *src_diff, tran_low_t *coeff, int diff_stride, TxfmParam *txfm_param);
void av1_lowbd_fwd_txfm_neon(const int16_t *src_diff, tran_low_t *coeff, int diff_stride, TxfmParam *txfm_param);
#define av1_lowbd_fwd_txfm av1_lowbd_fwd_txfm_neon

int64_t av1_lowbd_pixel_proj_error_c(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int xq[2], const sgr_params_type *params);
int64_t av1_lowbd_pixel_proj_error_neon(const uint8_t *src8, int width, int height, int src_stride, const uint8_t *dat8, int dat_stride, int32_t *flt0, int flt0_stride, int32_t *flt1, int flt1_stride, int xq[2], const sgr_params_type *params);
#define av1_lowbd_pixel_proj_error av1_lowbd_pixel_proj_error_neon

void av1_nn_fast_softmax_16_c(const float *input_nodes, float *output);
#define av1_nn_fast_softmax_16 av1_nn_fast_softmax_16_c

void av1_nn_predict_c(const float *input_nodes, const NN_CONFIG *const nn_config, int reduce_prec, float *const output);
void av1_nn_predict_neon(const float *input_nodes, const NN_CONFIG *const nn_config, int reduce_prec, float *const output);
#define av1_nn_predict av1_nn_predict_neon

void av1_quantize_b_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan, const qm_val_t * qm_ptr, const qm_val_t * iqm_ptr, int log_scale);
#define av1_quantize_b av1_quantize_b_c

void av1_quantize_fp_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
void av1_quantize_fp_neon(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
#define av1_quantize_fp av1_quantize_fp_neon

void av1_quantize_fp_32x32_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
void av1_quantize_fp_32x32_neon(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
#define av1_quantize_fp_32x32 av1_quantize_fp_32x32_neon

void av1_quantize_fp_64x64_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
void av1_quantize_fp_64x64_neon(const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr, const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
#define av1_quantize_fp_64x64 av1_quantize_fp_64x64_neon

void av1_quantize_lp_c(const int16_t *coeff_ptr, intptr_t n_coeffs, const int16_t *round_ptr, const int16_t *quant_ptr, int16_t *qcoeff_ptr, int16_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
void av1_quantize_lp_neon(const int16_t *coeff_ptr, intptr_t n_coeffs, const int16_t *round_ptr, const int16_t *quant_ptr, int16_t *qcoeff_ptr, int16_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr, const int16_t *scan, const int16_t *iscan);
#define av1_quantize_lp av1_quantize_lp_neon

void av1_resize_and_extend_frame_c(const YV12_BUFFER_CONFIG *src, YV12_BUFFER_CONFIG *dst, const InterpFilter filter, const int phase, const int num_planes);
void av1_resize_and_extend_frame_neon(const YV12_BUFFER_CONFIG *src, YV12_BUFFER_CONFIG *dst, const InterpFilter filter, const int phase, const int num_planes);
void av1_resize_and_extend_frame_neon_dotprod(const YV12_BUFFER_CONFIG *src, YV12_BUFFER_CONFIG *dst, const InterpFilter filter, const int phase, const int num_planes);
void av1_resize_and_extend_frame_neon_i8mm(const YV12_BUFFER_CONFIG *src, YV12_BUFFER_CONFIG *dst, const InterpFilter filter, const int phase, const int num_planes);
RTCD_EXTERN void (*av1_resize_and_extend_frame)(const YV12_BUFFER_CONFIG *src, YV12_BUFFER_CONFIG *dst, const InterpFilter filter, const int phase, const int num_planes);

void av1_resize_horz_dir_c(const uint8_t *const input, int in_stride, uint8_t *intbuf, int height, int filtered_length, int width2);
#define av1_resize_horz_dir av1_resize_horz_dir_c

bool av1_resize_vert_dir_c(uint8_t *intbuf, uint8_t *output, int out_stride, int height, int height2, int width2, int start_col);
#define av1_resize_vert_dir av1_resize_vert_dir_c

void av1_round_shift_array_c(int32_t *arr, int size, int bit);
void av1_round_shift_array_neon(int32_t *arr, int size, int bit);
#define av1_round_shift_array av1_round_shift_array_neon

int av1_selfguided_restoration_c(const uint8_t *dgd8, int width, int height,
                                  int dgd_stride, int32_t *flt0, int32_t *flt1, int flt_stride,
                                  int sgr_params_idx, int bit_depth, int highbd);
int av1_selfguided_restoration_neon(const uint8_t *dgd8, int width, int height,
                                  int dgd_stride, int32_t *flt0, int32_t *flt1, int flt_stride,
                                  int sgr_params_idx, int bit_depth, int highbd);
#define av1_selfguided_restoration av1_selfguided_restoration_neon

void av1_txb_init_levels_c(const tran_low_t *const coeff, const int width, const int height, uint8_t *const levels);
void av1_txb_init_levels_neon(const tran_low_t *const coeff, const int width, const int height, uint8_t *const levels);
#define av1_txb_init_levels av1_txb_init_levels_neon

void av1_upsample_intra_edge_c(uint8_t *p, int sz);
void av1_upsample_intra_edge_neon(uint8_t *p, int sz);
#define av1_upsample_intra_edge av1_upsample_intra_edge_neon

void av1_warp_affine_c(const int32_t *mat, const uint8_t *ref, int width, int height, int stride, uint8_t *pred, int p_col, int p_row, int p_width, int p_height, int p_stride, int subsampling_x, int subsampling_y, ConvolveParams *conv_params, int16_t alpha, int16_t beta, int16_t gamma, int16_t delta);
void av1_warp_affine_neon(const int32_t *mat, const uint8_t *ref, int width, int height, int stride, uint8_t *pred, int p_col, int p_row, int p_width, int p_height, int p_stride, int subsampling_x, int subsampling_y, ConvolveParams *conv_params, int16_t alpha, int16_t beta, int16_t gamma, int16_t delta);
void av1_warp_affine_neon_i8mm(const int32_t *mat, const uint8_t *ref, int width, int height, int stride, uint8_t *pred, int p_col, int p_row, int p_width, int p_height, int p_stride, int subsampling_x, int subsampling_y, ConvolveParams *conv_params, int16_t alpha, int16_t beta, int16_t gamma, int16_t delta);
RTCD_EXTERN void (*av1_warp_affine)(const int32_t *mat, const uint8_t *ref, int width, int height, int stride, uint8_t *pred, int p_col, int p_row, int p_width, int p_height, int p_stride, int subsampling_x, int subsampling_y, ConvolveParams *conv_params, int16_t alpha, int16_t beta, int16_t gamma, int16_t delta);

void av1_wedge_compute_delta_squares_c(int16_t *d, const int16_t *a, const int16_t *b, int N);
void av1_wedge_compute_delta_squares_neon(int16_t *d, const int16_t *a, const int16_t *b, int N);
#define av1_wedge_compute_delta_squares av1_wedge_compute_delta_squares_neon

int8_t av1_wedge_sign_from_residuals_c(const int16_t *ds, const uint8_t *m, int N, int64_t limit);
int8_t av1_wedge_sign_from_residuals_neon(const int16_t *ds, const uint8_t *m, int N, int64_t limit);
#define av1_wedge_sign_from_residuals av1_wedge_sign_from_residuals_neon

uint64_t av1_wedge_sse_from_residuals_c(const int16_t *r1, const int16_t *d, const uint8_t *m, int N);
uint64_t av1_wedge_sse_from_residuals_neon(const int16_t *r1, const int16_t *d, const uint8_t *m, int N);
#define av1_wedge_sse_from_residuals av1_wedge_sse_from_residuals_neon

void av1_wiener_convolve_add_src_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, const WienerConvolveParams *conv_params);
void av1_wiener_convolve_add_src_neon(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *filter_x, int x_step_q4, const int16_t *filter_y, int y_step_q4, int w, int h, const WienerConvolveParams *conv_params);
#define av1_wiener_convolve_add_src av1_wiener_convolve_add_src_neon

void cdef_copy_rect8_16bit_to_16bit_c(uint16_t *dst, int dstride, const uint16_t *src, int sstride, int width, int height);
void cdef_copy_rect8_16bit_to_16bit_neon(uint16_t *dst, int dstride, const uint16_t *src, int sstride, int width, int height);
#define cdef_copy_rect8_16bit_to_16bit cdef_copy_rect8_16bit_to_16bit_neon

void cdef_copy_rect8_8bit_to_16bit_c(uint16_t *dst, int dstride, const uint8_t *src, int sstride, int width, int height);
void cdef_copy_rect8_8bit_to_16bit_neon(uint16_t *dst, int dstride, const uint8_t *src, int sstride, int width, int height);
#define cdef_copy_rect8_8bit_to_16bit cdef_copy_rect8_8bit_to_16bit_neon

void cdef_filter_16_0_c(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_16_0_neon(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_16_0 cdef_filter_16_0_neon

void cdef_filter_16_1_c(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_16_1_neon(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_16_1 cdef_filter_16_1_neon

void cdef_filter_16_2_c(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_16_2_neon(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_16_2 cdef_filter_16_2_neon

void cdef_filter_16_3_c(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_16_3_neon(void *dst16, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_16_3 cdef_filter_16_3_neon

void cdef_filter_8_0_c(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_8_0_neon(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_8_0 cdef_filter_8_0_neon

void cdef_filter_8_1_c(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_8_1_neon(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_8_1 cdef_filter_8_1_neon

void cdef_filter_8_2_c(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_8_2_neon(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_8_2 cdef_filter_8_2_neon

void cdef_filter_8_3_c(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
void cdef_filter_8_3_neon(void *dst8, int dstride, const uint16_t *in, int pri_strength, int sec_strength, int dir, int pri_damping, int sec_damping, int coeff_shift, int block_width, int block_height);
#define cdef_filter_8_3 cdef_filter_8_3_neon

int cdef_find_dir_c(const uint16_t *img, int stride, int32_t *var, int coeff_shift);
int cdef_find_dir_neon(const uint16_t *img, int stride, int32_t *var, int coeff_shift);
#define cdef_find_dir cdef_find_dir_neon

void cdef_find_dir_dual_c(const uint16_t *img1, const uint16_t *img2, int stride, int32_t *var1, int32_t *var2, int coeff_shift, int *out1, int *out2);
void cdef_find_dir_dual_neon(const uint16_t *img1, const uint16_t *img2, int stride, int32_t *var1, int32_t *var2, int coeff_shift, int *out1, int *out2);
#define cdef_find_dir_dual cdef_find_dir_dual_neon

cfl_subsample_hbd_fn cfl_get_luma_subsampling_420_hbd_c(TX_SIZE tx_size);
cfl_subsample_hbd_fn cfl_get_luma_subsampling_420_hbd_neon(TX_SIZE tx_size);
#define cfl_get_luma_subsampling_420_hbd cfl_get_luma_subsampling_420_hbd_neon

cfl_subsample_lbd_fn cfl_get_luma_subsampling_420_lbd_c(TX_SIZE tx_size);
cfl_subsample_lbd_fn cfl_get_luma_subsampling_420_lbd_neon(TX_SIZE tx_size);
#define cfl_get_luma_subsampling_420_lbd cfl_get_luma_subsampling_420_lbd_neon

cfl_subsample_hbd_fn cfl_get_luma_subsampling_422_hbd_c(TX_SIZE tx_size);
cfl_subsample_hbd_fn cfl_get_luma_subsampling_422_hbd_neon(TX_SIZE tx_size);
#define cfl_get_luma_subsampling_422_hbd cfl_get_luma_subsampling_422_hbd_neon

cfl_subsample_lbd_fn cfl_get_luma_subsampling_422_lbd_c(TX_SIZE tx_size);
cfl_subsample_lbd_fn cfl_get_luma_subsampling_422_lbd_neon(TX_SIZE tx_size);
#define cfl_get_luma_subsampling_422_lbd cfl_get_luma_subsampling_422_lbd_neon

cfl_subsample_hbd_fn cfl_get_luma_subsampling_444_hbd_c(TX_SIZE tx_size);
cfl_subsample_hbd_fn cfl_get_luma_subsampling_444_hbd_neon(TX_SIZE tx_size);
#define cfl_get_luma_subsampling_444_hbd cfl_get_luma_subsampling_444_hbd_neon

cfl_subsample_lbd_fn cfl_get_luma_subsampling_444_lbd_c(TX_SIZE tx_size);
cfl_subsample_lbd_fn cfl_get_luma_subsampling_444_lbd_neon(TX_SIZE tx_size);
#define cfl_get_luma_subsampling_444_lbd cfl_get_luma_subsampling_444_lbd_neon

cfl_predict_hbd_fn cfl_get_predict_hbd_fn_c(TX_SIZE tx_size);
cfl_predict_hbd_fn cfl_get_predict_hbd_fn_neon(TX_SIZE tx_size);
#define cfl_get_predict_hbd_fn cfl_get_predict_hbd_fn_neon

cfl_predict_lbd_fn cfl_get_predict_lbd_fn_c(TX_SIZE tx_size);
cfl_predict_lbd_fn cfl_get_predict_lbd_fn_neon(TX_SIZE tx_size);
#define cfl_get_predict_lbd_fn cfl_get_predict_lbd_fn_neon

cfl_subtract_average_fn cfl_get_subtract_average_fn_c(TX_SIZE tx_size);
cfl_subtract_average_fn cfl_get_subtract_average_fn_neon(TX_SIZE tx_size);
#define cfl_get_subtract_average_fn cfl_get_subtract_average_fn_neon

void av1_rtcd(void);

#include "config/aom_config.h"

#ifdef RTCD_C
#include "aom_ports/arm.h"
static void setup_rtcd_internal(void)
{
    int flags = aom_arm_cpu_caps();

    (void)flags;

    av1_apply_temporal_filter = av1_apply_temporal_filter_neon;
    if (flags & HAS_NEON_DOTPROD) av1_apply_temporal_filter = av1_apply_temporal_filter_neon_dotprod;
    av1_convolve_2d_scale = av1_convolve_2d_scale_neon;
    if (flags & HAS_NEON_DOTPROD) av1_convolve_2d_scale = av1_convolve_2d_scale_neon_dotprod;
    if (flags & HAS_NEON_I8MM) av1_convolve_2d_scale = av1_convolve_2d_scale_neon_i8mm;
    av1_convolve_2d_sr = av1_convolve_2d_sr_neon;
    if (flags & HAS_NEON_DOTPROD) av1_convolve_2d_sr = av1_convolve_2d_sr_neon_dotprod;
    if (flags & HAS_NEON_I8MM) av1_convolve_2d_sr = av1_convolve_2d_sr_neon_i8mm;
    av1_convolve_x_sr = av1_convolve_x_sr_neon;
    if (flags & HAS_NEON_DOTPROD) av1_convolve_x_sr = av1_convolve_x_sr_neon_dotprod;
    if (flags & HAS_NEON_I8MM) av1_convolve_x_sr = av1_convolve_x_sr_neon_i8mm;
    av1_convolve_y_sr = av1_convolve_y_sr_neon;
    if (flags & HAS_NEON_DOTPROD) av1_convolve_y_sr = av1_convolve_y_sr_neon_dotprod;
    if (flags & HAS_NEON_I8MM) av1_convolve_y_sr = av1_convolve_y_sr_neon_i8mm;
    av1_dist_wtd_convolve_2d = av1_dist_wtd_convolve_2d_neon;
    if (flags & HAS_NEON_DOTPROD) av1_dist_wtd_convolve_2d = av1_dist_wtd_convolve_2d_neon_dotprod;
    if (flags & HAS_NEON_I8MM) av1_dist_wtd_convolve_2d = av1_dist_wtd_convolve_2d_neon_i8mm;
    av1_dist_wtd_convolve_x = av1_dist_wtd_convolve_x_neon;
    if (flags & HAS_NEON_DOTPROD) av1_dist_wtd_convolve_x = av1_dist_wtd_convolve_x_neon_dotprod;
    if (flags & HAS_NEON_I8MM) av1_dist_wtd_convolve_x = av1_dist_wtd_convolve_x_neon_i8mm;
    av1_get_crc32c_value = av1_get_crc32c_value_c;
    if (flags & HAS_ARM_CRC32) av1_get_crc32c_value = av1_get_crc32c_value_arm_crc32;
    av1_resize_and_extend_frame = av1_resize_and_extend_frame_neon;
    if (flags & HAS_NEON_DOTPROD) av1_resize_and_extend_frame = av1_resize_and_extend_frame_neon_dotprod;
    if (flags & HAS_NEON_I8MM) av1_resize_and_extend_frame = av1_resize_and_extend_frame_neon_i8mm;
    av1_warp_affine = av1_warp_affine_neon;
    if (flags & HAS_NEON_I8MM) av1_warp_affine = av1_warp_affine_neon_i8mm;
}
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AV1_RTCD_H_
