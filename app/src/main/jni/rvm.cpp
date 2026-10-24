// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2025 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "rvm.h"

#include "net.h"

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <cmath>

RVM::RVM()
{
    model_type = 0;
    target_size = 512;
    intra_inter = 0;
    segmentation = false;
    refine_deep = true;
    refine_fast = false;

    // Precompute gamma lookup table: pow(v / 255.0, 0.9) * 255.0
    for (int i = 0; i < 256; i++)
    {
        double v = i / 255.0;
        double g = std::pow(v, 0.9);
        gamma_lut[i] = cv::saturate_cast<uchar>(g * 255.0 + 0.5);
    }
}

RVM::~RVM()
{
}

void RVM::reset_temporal_state()
{
    prev_pha.release();
}

int RVM::load(const char* parampath, const char* modelpath, bool use_gpu)
{
    rvm.clear();

    rvm.opt.use_fp16_packed = false;
    rvm.opt.use_fp16_storage = false;
    rvm.opt.use_fp16_arithmetic = false;

#if NCNN_VULKAN
    rvm.opt.use_vulkan_compute = use_gpu;
#endif

    rvm.load_param(parampath);
    rvm.load_model(modelpath);

    reset_temporal_state();
    return 0;
}

int RVM::load(AAssetManager* mgr, const char* parampath, const char* modelpath, bool use_gpu)
{
    rvm.clear();

    rvm.opt.use_fp16_packed = false;
    rvm.opt.use_fp16_storage = false;
    rvm.opt.use_fp16_arithmetic = false;

#if NCNN_VULKAN
    rvm.opt.use_vulkan_compute = use_gpu;
#endif

    rvm.load_param(mgr, parampath);
    rvm.load_model(mgr, modelpath);

    reset_temporal_state();
    return 0;
}

void RVM::set_model_type(int _model_type)
{
    model_type = _model_type;
}

void RVM::set_target_size(int _target_size)
{
    target_size = _target_size;
}

void RVM::set_intra_inter(int _intra_inter)
{
    intra_inter = _intra_inter;
}

void RVM::set_postproc_mode(bool _segmentation, bool _refine_deep, bool _refine_fast)
{
    segmentation = _segmentation;
    refine_deep = _refine_deep;
    refine_fast = _refine_fast;
}

int RVM::detect(const cv::Mat& rgb, InterFeatures& feats, cv::Mat& fgr, cv::Mat& pha, cv::Mat& seg)
{
    const int w = rgb.cols;
    const int h = rgb.rows;

    const int max_stride = 16;

    const float norm_vals[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};

    ncnn::Mat in_pad;
    ncnn::Mat in_small_pad;

    int wpad = 0;
    int hpad = 0;

    bool downsample = std::max(w, h) > target_size;
    if (downsample)
    {
        // letterbox pad to multiple of max_stride
        int w2 = w;
        int h2 = h;
        float scale = 1.f;
        if (w > h)
        {
            scale = (float)target_size / w;
            w2 = target_size;
            h2 = h2 * scale;
        }
        else
        {
            scale = (float)target_size / h;
            h2 = target_size;
            w2 = w2 * scale;
        }

        ncnn::Mat in_small = ncnn::Mat::from_pixels_resize(rgb.data, ncnn::Mat::PIXEL_RGB, w, h, w2, h2);

        // letterbox pad to target_size rectangle
        int w2pad = (w2 + max_stride - 1) / max_stride * max_stride - w2;
        int h2pad = (h2 + max_stride - 1) / max_stride * max_stride - h2;
        ncnn::copy_make_border(in_small, in_small_pad, h2pad / 2, h2pad - h2pad / 2, w2pad / 2, w2pad - w2pad / 2, ncnn::BORDER_CONSTANT, 114.f);

        in_small_pad.substract_mean_normalize(0, norm_vals);

        int w3 = w;
        int h3 = h;
        if (w > h)
        {
            w3 = w;
            h3 = in_small_pad.h / scale;
            wpad = 0;
            hpad = h3 - h;
        }
        else
        {
            h3 = h;
            w3 = in_small_pad.w / scale;
            wpad = w3 - w;
            hpad = 0;
        }

        ncnn::Mat in = ncnn::Mat::from_pixels(rgb.data, ncnn::Mat::PIXEL_RGB, w, h);

        ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 114.f);

        in_pad.substract_mean_normalize(0, norm_vals);
    }
    else
    {
        ncnn::Mat in = ncnn::Mat::from_pixels(rgb.data, ncnn::Mat::PIXEL_RGB, w, h);

        // letterbox pad to target_size rectangle
        wpad = (w + max_stride - 1) / max_stride * max_stride - w;
        hpad = (h + max_stride - 1) / max_stride * max_stride - h;
        ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 114.f);

        in_pad.substract_mean_normalize(0, norm_vals);

        in_small_pad = in_pad;
    }

    bool is_first_frame = false;
    if (feats.r1.empty() && feats.r2.empty() && feats.r3.empty() && feats.r4.empty())
    {
        is_first_frame = true;
    }

    if (model_type == 0)
    {
        // rvm_mobilenetv3
        feats.r1.create(in_small_pad.w / 2, in_small_pad.h / 2, 16);
        feats.r2.create(in_small_pad.w / 4, in_small_pad.h / 4, 20);
        feats.r3.create(in_small_pad.w / 8, in_small_pad.h / 8, 40);
        feats.r4.create(in_small_pad.w / 16, in_small_pad.h / 16, 64);
    }
    else // if (model_type == 1)
    {
        // rvm_resnet50
        feats.r1.create(in_small_pad.w / 2, in_small_pad.h / 2, 16);
        feats.r2.create(in_small_pad.w / 4, in_small_pad.h / 4, 32);
        feats.r3.create(in_small_pad.w / 8, in_small_pad.h / 8, 64);
        feats.r4.create(in_small_pad.w / 16, in_small_pad.h / 16, 128);
    }

    if (intra_inter == 0 || is_first_frame)
    {
        feats.r1.fill(0.f);
        feats.r2.fill(0.f);
        feats.r3.fill(0.f);
        feats.r4.fill(0.f);
    }

    ncnn::Extractor ex = rvm.create_extractor();

    ex.input("in0", in_pad);
    ex.input("in1", in_small_pad);

    ex.input("in2", feats.r1);
    ex.input("in3", feats.r2);
    ex.input("in4", feats.r3);
    ex.input("in5", feats.r4);

    ncnn::Mat out_fgr;
    ncnn::Mat out_pha;
    ncnn::Mat out_seg;

    if (segmentation)
    {
        // segmentation
        ex.extract("out6", out_seg);
    }
    else if (downsample)
    {
        if (refine_deep)
        {
            // downsample + refine deep
            ex.extract("out2", out_fgr);
            ex.extract("out3", out_pha);
        }
        else // if (refine_fast)
        {
            // downsample + refine fast
            ex.extract("out4", out_fgr);
            ex.extract("out5", out_pha);
        }
    }
    else
    {
        // no downsample
        ex.extract("out0", out_fgr);
        ex.extract("out1", out_pha);
    }

    if (intra_inter == 1)
    {
        // feats
        ex.extract("out7", feats.r1, 1);
        ex.extract("out8", feats.r2, 1);
        ex.extract("out9", feats.r3, 1);
        ex.extract("out10", feats.r4, 1);
    }

    const float denorm_vals[3] = {255.f, 255.f, 255.f};

    if (segmentation)
    {
        out_seg.substract_mean_normalize(0, denorm_vals);
        seg.create(in_pad.h, in_pad.w, CV_8UC1);
        out_seg.to_pixels_resize(seg.data, ncnn::Mat::PIXEL_GRAY, in_pad.w, in_pad.h);

        // cut letterbox pad
        seg = seg(cv::Rect(wpad / 2, hpad / 2, w, h));
    }
    else
    {
        out_fgr.substract_mean_normalize(0, denorm_vals);
        fgr.create(out_fgr.h, out_fgr.w, CV_8UC3);
        out_fgr.to_pixels(fgr.data, ncnn::Mat::PIXEL_RGB);

        out_pha.substract_mean_normalize(0, denorm_vals);
        pha.create(out_pha.h, out_pha.w, CV_8UC1);
        out_pha.to_pixels(pha.data, ncnn::Mat::PIXEL_GRAY);

        // cut letterbox pad
        fgr = fgr(cv::Rect(wpad / 2, hpad / 2, w, h));
        pha = pha(cv::Rect(wpad / 2, hpad / 2, w, h));
    }

    return 0;
}

// Alpha post-processing pipeline:
// 1. EMA temporal smoothing: alpha = 0.7 * alpha_prev + 0.3 * alpha_curr
// 2. Gaussian blur (1px kernel/sigma ~0.5)
// 3. Morphological close (2px kernel)
// 4. Erode (1px kernel)
// 5. Gamma correction: pow(alpha, 0.9) via LUT
int RVM::postprocess_alpha(cv::Mat& pha)
{
    if (pha.empty()) return 0;

    // 1. EMA temporal smoothing
    if (!prev_pha.empty() && prev_pha.size() == pha.size())
    {
        // alpha_curr = 0.7 * prev + 0.3 * curr
        cv::addWeighted(prev_pha, 0.7, pha, 0.3, 0, pha);
    }
    pha.copyTo(prev_pha);

    // 2. Gaussian blur (3x3 kernel softens edges)
    cv::GaussianBlur(pha, pha, cv::Size(3, 3), 0.5);

    // 3. Morphological close (5x5 kernel fills tiny holes)
    static cv::Mat close_kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
    cv::morphologyEx(pha, pha, cv::MORPH_CLOSE, close_kernel);

    // 4. Erode (3x3 kernel removes dark halo)
    static cv::Mat erode_kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::erode(pha, pha, erode_kernel);

    // 5. Gamma correction via precomputed LUT: pow(val / 255.0, 0.9)
    int rows = pha.rows;
    int cols = pha.cols;
    if (pha.isContinuous())
    {
        cols *= rows;
        rows = 1;
    }

    for (int y = 0; y < rows; y++)
    {
        uchar* p = pha.ptr<uchar>(y);
        for (int x = 0; x < cols; x++)
        {
            p[x] = gamma_lut[p[x]];
        }
    }

    return 0;
}

int RVM::draw(cv::Mat& rgb, const cv::Mat& fgr, const cv::Mat& pha, const cv::Mat& seg)
{
    return draw_magenta(rgb, fgr, pha);
}

// Composites foreground person onto solid magenta background (#FF00FF: R=255, G=0, B=255)
int RVM::draw_magenta(cv::Mat& rgb, const cv::Mat& fgr, const cv::Mat& pha)
{
    const int w = rgb.cols;
    const int h = rgb.rows;

    if (!fgr.empty() && !pha.empty())
    {
        for (int y = 0; y < h; y++)
        {
            const uchar* pf = fgr.ptr<const uchar>(y);
            const uchar* pa = pha.ptr<const uchar>(y);
            uchar* p = rgb.ptr<uchar>(y);
            for (int x = 0; x < w; x++)
            {
                const float alpha = pa[0] / 255.f;
                // Magenta background: RGB = (255, 0, 255)
                p[0] = cv::saturate_cast<uchar>(pf[0] * alpha + (1.0f - alpha) * 255.0f); // R
                p[1] = cv::saturate_cast<uchar>(pf[1] * alpha + (1.0f - alpha) * 0.0f);   // G
                p[2] = cv::saturate_cast<uchar>(pf[2] * alpha + (1.0f - alpha) * 255.0f); // B
                pf += 3;
                pa += 1;
                p += 3;
            }
        }
    }

    return 0;
}
